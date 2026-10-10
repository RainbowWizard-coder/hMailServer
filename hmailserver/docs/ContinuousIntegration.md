Continuous integration
======================

GitLab CI starts every pipeline (`.gitlab-ci.yml` at the repository root), and
every job of a pipeline for `master`, a `batch*` branch, a `v*` tag or a
schedule runs on the project's own runners - linix and bench150, and since 5
October 2026 server2 beside linix ([The runners](#the-runners)). The checks run
for every push and every merge request; the builds, both regression suites and
everything else that needs a machine of the project's run only for the refs that
only a maintainer can push. The owner decided on GitLab CI with runners of the
project's on 25 September 2026, a week after GitHub suspended the project's
account and its workflows with it. The same day, the rest of the old GitHub
workflows were ported ([From GitHub Actions to GitLab
CI](#from-github-actions-to-gitlab-ci)), and a nightly fuzz pipeline was added
([The fuzz schedule](#the-fuzz-schedule)). Until 9 October 2026 the checks, the
change gates and the release's signing and publishing jobs ran on GitLab's
shared runners, on the namespace's compute minutes; those ran out that day, and
since then every job of a pipeline for a protected ref that is not a merge
request's runs on the project's runners, and only a merge request's pipeline and
an unprotected branch's run their checks on GitLab's hosted runners, which the
project's take nothing of ([Every job on the project's own
runners](#every-job-on-the-projects-own-runners)).

**The jobs have run since 26 September 2026.** The first pipeline, 2884863496
on `master`, failed eleven of its jobs ([What the first pipeline found](#what-the-first-pipeline-found));
`master`'s pipeline 2886389460 on 27 September passed 25 of its 29, failed
`secret_detection`, `semgrep` and `static-analysis`, and left `db-matrix`
manual. The release jobs first ran for 6.3.5, whose tag pipeline 2892097590
published the release on 29 September 2026 ([The release](#the-release)). This
page was written before either runner had taken a job: the ported Linux jobs'
times below are those of their scripts run by hand on linix, in the jobs' own
images and beside the same services, and every other duration is an estimate
made from the gates run by hand, and says so, but for the times 6.3.5's
pipeline measured ([How long each job takes](#how-long-each-job-takes)). A
pipeline's own times are on its jobs' pages.

What runs where
---------------

*Runs on* names where GitLab sends a job in a pipeline for a protected ref that is
not a merge request's: bench150, linix's second entry `linix-systemd`, or the tag
`linix`, which two runners carry since 5 October 2026, linix and server2, so that
such a job runs on whichever of the two takes it first ([The runners](#the-runners)).
A job that names no runner of its own takes `linix` from `default:`; in a merge
request's pipeline and an unprotected branch's that is GitLab's hosted runner
instead ([Every job on the project's own runners](#every-job-on-the-projects-own-runners)).
The other tables on this page name runners the same way.

| Job | Stage | Runs on | Runs for | What it does |
|---|---|---|---|---|
| `checkers` | check | linix | every pipeline but a schedule's | Every Python checker, the self-tests, the generators' `--check` ([CONTRIBUTING.md](../../CONTRIBUTING.md), "The checkers") |
| `harnesses` | check | linix | every pipeline but a schedule's | The Control Deck's and the webmail's script harnesses, and the S/MIME and OpenPGP checks |
| `sign-off` | check | linix | every merge request, and a `batch*` branch | Every commit the merge request or the batch adds to `master` carries its author's `Signed-off-by` (`build/sign-off.py`; [Landing on master](#landing-on-master)) |
| `editorconfig` | check | linix | every pipeline | Every text file against `.editorconfig`, and a negative control |
| `chart-and-compose` | check | linix | a change to the Helm chart or the Compose file | The chart linted and rendered, the Compose file resolved |
| `update-helper` | check | linix | a change to the update helper or the units | `build/test-hmailserver-update.sh` under dash, and `systemd-analyze verify` of the three units |
| `package-templates` | check | linix | a change to the winget or Chocolatey templates | The templates render and agree, checked offline |
| `roadmap` | check | linix | a change to `Roadmap.md` | `build/check-roadmap.ps1` |
| `cli-packages` | check | linix | a change to the command-line clients or their packaging, and never a tag | `hmailserver-cli`'s sdist and wheel built twice and compared, held to the tree, `twine check`ed and installed alone (`.gitlab/ci/cli-packages.yml`; [CommandLinePackages.md](CommandLinePackages.md)) |
| `cli-powershell` | check | linix | the same | The PowerShell module's manifest and PSScriptAnalyzer from the Gallery, under pwsh (`build/ci/cli-powershell.ps1`), and the module's half of `build/check-cli.py` against its recorded server |
| `linux-build` | build | linix | a protected ref | The Linux server with clang, every `*-tests` program the CMake build makes, the OpenPGP writer against GnuPG, and the `.deb` and the `.rpm` (`build/ci/linux-build.sh`) |
| `linux-build-gcc` | test | linix | `master` or a `batch*` branch; never a tag | The same server and tester programs built with GCC, the distribution's default compiler, and the testers and the OpenPGP round trip run; no packages (`.gitlab/ci/gcc.yml`; [The GCC build](#the-gcc-build)) |
| `nix` | build | linix | `master` or a `batch*` branch; never a tag | The project's Nix flake: `flake.lock` held to releases.nixos.org, every output evaluated for both systems, `nix build .#hmailserver` with its tester programs, and the NixOS test under QEMU's TCG, since linix has no KVM (`.gitlab/ci/nix.yml`, `build/ci/nix.sh`; [Nix.md](Nix.md)) |
| `alpine-package` | build | linix | `master` or a `batch*` branch; never a tag | The server built with musl by its APKBUILD and abuild on Alpine 3.24 from the source tarball, `check()` running its tester programs, and `apkbuild-lint` (`.gitlab/ci/alpine.yml`, `build/ci/alpine.sh`; [Alpine.md](Alpine.md)) |
| `alpine-install` | test | linix | the same | Those packages installed in a clean Alpine container, the server started with `rc-service` against PostgreSQL, SMTP (with STARTTLS) and IMAP asked, then a reload and a stop |
| `alpine-suite` | test | linix | the same | The whole of `LinuxRegressionTests` against the installed Alpine server, in Alpine, on PostgreSQL 18 |
| `pages` | release | linix | `master`, a `v*` tag after `release-publish`, a schedule on `master` | Renders the update feed from the project's releases into `public/`; it deploys nothing itself (`pages: false`, from `.gitlab/ci/package-repos.yml`) (see [The update feed](#the-update-feed)) |
| `package-repos` | release | linix | wherever `pages` runs, after it | Adds the signed apt and dnf repositories to `pages`' `public/`, or, without the signing key, nothing, and the project's OpenSSF Scorecard result when there is one ([OpenSSF Scorecard](#openssf-scorecard)), and GitLab Pages publishes the result at https://updates.progressiverobot.com ([PackageManagers.md](PackageManagers.md), "apt and dnf") |
| `linux-suite` | test | linix | a protected ref | The whole of `LinuxRegressionTests` against that server on PostgreSQL 18 (`build/ci/linux-suite.sh`) |
| `linux-cross` | test | linix | a protected ref | Every core translation unit compiled for AArch64 (`build/ci/linux-cross.sh`) |
| `linux-sanitize` | test | linix | a protected ref | `snmp-tests` under AddressSanitizer; says SKIPPED and passes while the tree has no `HM_SANITIZE` (`build/ci/linux-sanitize.sh`) |
| `linux-db-upgrade-pgsql`, `linux-db-upgrade-mysql` | test | linix | a protected ref | A database made by 6029's own create script, with orphaned rows, upgrades to this schema and to the schema of a new database, on PostgreSQL 18 and MariaDB 13 (`build/ci/linux-db-upgrade.sh`) |
| `linux-package-deb`, `linux-package-rpm` | test | linix | a protected ref | The packages installed in clean Ubuntu 26.04 and Fedora 44 containers; the `.deb` configured, started and asked for the Deck (`build/ci/linux-package-install.sh`) |
| `linux-image` | test | linix | a protected ref | The container image for linux/amd64 built from the `.deb` with kaniko and inspected, not pushed (`build/ci/linux-image.sh`) |
| `linux-image-arm64` | test | linix | a protected ref, once `deb-ubuntu26.04-arm64` has passed (on `master` that build is started by hand) | The container image for linux/arm64 built with kaniko from that job's `.deb`, its `RUN` under qemu-user, and inspected as `linux-image` inspects its own, not pushed (`.gitlab/ci/container-image.yml`, `build/ci/linux-image.sh` with `HM_IMAGE_ARCH=arm64`) |
| `linux-sbom` | test | linix | a protected ref | The SPDX and CycloneDX SBOMs of the tree, kept 90 days (`build/ci/linux-sbom.sh`) |
| `linux-repro` | test | linix | a protected ref, after `linux-build` | `linux-build`'s binary, `.deb` and `.rpm` made a second time from a copy of the tree at another path with other file modes, in another build directory, started under another umask and in another time zone, and compared: all three must be byte for byte identical (`build/ci/linux-repro.sh`; [ReproducibleBuilds.md](ReproducibleBuilds.md)) |
| `db-matrix` | test | linix | a protected ref, started by hand | The `linux-build` server against MySQL 8.4, MariaDB 11.8 or PostgreSQL 17 in the test lab (`HM_LAB_DB`), the database-sensitive fixtures or the whole suite (`HM_SUITE_FILTER`); `build/lab/db-suite.sh`, [TestLab.md](TestLab.md) |
| `windows-gate` | test | bench150 | a protected ref | The whole Windows suite in the bench's own checkout (`build/ci/windows-gate.ps1`) |
| `windows-dotnet` | test | bench150 | a protected ref | The Control Panel and the tools built with warnings as errors, published and tested, and `dotnet format` (`build/ci/windows-dotnet.ps1`) |
| `build-inputs-authenticode` | build | bench150 | a protected ref | Every build input downloaded afresh and held to its pinned SHA-256, and every third-party file the installers take from one held to the Authenticode record `third-party-binaries.json` keeps for it: signed by the signer recorded, with a timestamp, or unsigned (`build/ci/build-inputs-authenticode.ps1`); then that check and `release-authenticode`'s pin tested, each refusing what it must (`build/ci/authenticode-selftest.ps1`). `windows-packages-x64`, `windows-packages-arm64` and `windows-installer` need it ([ThirdPartyBinaries.md](ThirdPartyBinaries.md), "What checks it") |
| `linux-fuzz` | test | linix | the schedule described `fuzz`, and nothing else | The three MIME harnesses under libFuzzer (`build/ci/linux-fuzz.sh`); its findings are its artifact, for Developers and above |
| `linux-coverage` | test | linix | the schedule described `coverage`, and nothing else | The server and its tester programs built with clang's source-based coverage, the testers and the whole Linux suite run against it, and llvm-cov's line, region and branch figures (`build/ci/linux-coverage.sh`, [The coverage schedule](#the-coverage-schedule)) |
| `static-analysis` | test | linix | a protected ref; a `batch*` branch by what it changes from `master` ([Change checks](#change-checks)) | cppcheck and clang-tidy over the server's C++, held to a baseline and to `master`'s exceptions file ([Security scanning](#security-scanning)) |
| `semgrep` | test | linix | the same | Semgrep's open rules over the C#, Python, JavaScript and shell sources, held to a baseline and to `master`'s exceptions file |
| `ci-image-packages` | test | linix | the same | Copies the CI image's package database out, for `dependency-scan` |
| `dependency-scan` | test | linix | the same | Trivy over the lock files and the CI image, OSV over the vendored libraries |
| `repo-hygiene` | test | linix | a protected ref | The repository checks OpenSSF Scorecard ran, answered from GitLab and the tree |
| `semgrep-sast` | test | linix | every change - a merge request, and a protected `batch*` branch - `master`, and a `v*` tag | GitLab's own SAST template: its Semgrep analyzer and rules over the C, C++, C#, Python and JavaScript - less what lies under a `build`, `dist`, `vendor` or `node_modules` directory, which the analyzer's template leaves out, so not the scripts under `build/` - with no baseline, in the project's copy of the analyzer's image; not `allow_failure`, and it keeps Semgrep's SARIF beside its report (`.gitlab/ci/gitlab-security.yml`) |
| `semgrep-sast-gate` | test | linix | every change: a merge request, and a protected `batch*` branch | `semgrep-sast`'s report read by the release's rule, for the files the change touches from its base, or every file when it alters the scan: a Critical or High finding that `.gitlab/security-exceptions.yml`, as the base has it, does not accept fails it, and so does a baseline that grows or loses a reason, a suppression comment added, a narrowing of what a scan reads that does not land alone, a source file put out of the scan's reach, a changed file a rule gave up on, and a report that is not this pipeline's or that the tree commits (`build/ci/security-gate.py --merge-request auto`; [Change checks](#change-checks)) |
| `mr-dependency-check` | check | linix | every change: a merge request, and a protected `batch*` branch | Every NuGet and PyPI version the change adds, looked up in OSV's own export of its ecosystem's advisories, and every native or vendored library pin it moves, asked of OSV by its upstream tag: a malicious package, or a Critical or High advisory, that neither the base's exceptions nor its VEX document accepts fails it (`build/ci/mr-dependency-check.py --base auto`) |
| `secret_detection` | test | linix | `master`, a merge request, and the schedule described `security` | GitLab's own secret-detection template: a key, token or password in the commits the pipeline brings, or in the whole history for the `security` schedule - with GitLab's rules but for the two entries of their allowlist that leave out `build/` and the like, so the scripts under `build/` too ([What the secret scans read](#what-the-secret-scans-read)) - in the project's copy of the analyzer's image; a finding `.gitleaksignore` does not name fails it (`.gitlab/ci/gitlab-security.yml`) |
| `windows-installer` | release | bench150 | a `v*` tag, after `windows-gate` | The x64 Windows installer, unsigned, built in the bench's checkout (`build/ci/windows-installer.ps1`; [The release](#the-release)); the ARM64 one is `windows-packages-arm64`'s ([PackagesWindows.md](PackagesWindows.md)) |
| `release` | release | linix | a `v*` tag, started by hand | Checks the tag, the version stamp and the release settings, reads the four security scans' reports (`build/ci/security-gate.py`), collects the sixty files a release carries before the two installers' SBOMs, the PowerShell module's two unsigned among them (eight in 6.3.5), and counts what GitLab and Pages will take of it (`build/ci/release-chain.py collect`) |
| `release-authenticode` | release | bench150 | a `v*` tag, after `release` | The two installers, x64 and ARM64, and the PowerShell module's manifest and script, Authenticode-signed through Azure Artifact Signing with the job's ID token (`build/ci/windows-authenticode.ps1`) |
| `release-sbom` | release | linix | a `v*` tag, after `release-authenticode` | Each signed installer's own SBOM, `<installer>.cdx.json`, and the statement of the release's SLSA provenance (`build/ci/release-chain.py sbom`, `build/asset-sbom.py`) |
| `release-sign` | release | linix | a `v*` tag, after `release-sbom` | Every file signed with Sigstore as this pipeline, each primary file's bundle copied to `<file>.sigstore.json`, and the provenance attested (`build/ci/release-sign.sh`) |
| `release-verify` | release | linix | a `v*` tag, after `release-sign` | Every bundle checked with this commit's `hmailserver --verify-update`, and the SBOMs, the copies and the provenance held to the files (`build/ci/release-verify.sh`) |
| `release-publish` | release | linix | a `v*` tag, started by hand after `release-verify` | The files uploaded to the package registry and the GitLab release created (`build/ci/release-chain.py publish`) |
| `release-packages` | test | linix | a `v*` tag or a `batch*` branch, where the package jobs run, after every package build it gathers and its install or test job | Every distribution release's `.deb` and `.rpm`, both AppImages, the source of the libraries linked into their runtime, the source of the libraries they carry in `usr/lib` and the source tarball, each held to its build's sums (the tarball to `build/check-source-tarball.py`) and each but the two sources files given its SBOM, for `release` and the rehearsal (`.gitlab/ci/release-packages.yml`, `build/ci/release-chain.py gather`; [What a release carries](#the-release)) |
| `release-rehearsal`, `release-rehearsal-sign`, `release-rehearsal-verify` | release | linix; linix, in cosign's image; linix | a protected `batch*` branch, where the package jobs run; never a tag, `master` or a merge request | The release rehearsed: collected, signed as the branch and verified, published nowhere (`.gitlab/ci/release-rehearsal.yml`; [The release rehearsal](#the-release-rehearsal)) |
| `helm-chart-package` | build | linix | a `v*` tag or a `batch*` branch | The Helm chart packaged at the release's version as `hmailserver-chart-<version>.tgz`, a file of the release (`.gitlab/ci/helm-chart.yml`, `build/ci/helm-chart-package.sh`; [ContainerImage.md](ContainerImage.md)) |
| `container-image-deb` | release | linix | a `v*` tag, started by hand after `release-publish`; refuses unless the project's variable `HM_PUBLISH_IMAGE` is `1` | The release's `.deb` of each architecture, amd64 and arm64, and their SBOMs taken from the published release and verified against their Sigstore bundles (`build/ci/container-image-deb.sh`; [The container image](#the-container-image)) |
| `container-image-amd64`, `container-image-arm64` | release | linix | a `v*` tag, after `container-image-deb`; refuse unless `HM_PUBLISH_IMAGE` is `1` | Each platform's image built with kaniko from its own architecture's `.deb` - the arm64 one under qemu-user - inspected as `linux-image` inspects its own, and kept for `container-image`, not pushed (`build/ci/linux-image.sh` with `HM_IMAGE_RELEASE=1`) |
| `container-image` | release | linix, in regctl's image | a `v*` tag, after both; refuses unless `HM_PUBLISH_IMAGE` is `1` | Both images, each held to what its job checked, joined under one index and pushed to the project's container registry: each image by its digest, the index with the release's tags (`build/ci/container-image-push.sh`) |
| `container-image-artifacthub` | release | linix, in regctl's image | a `v*` tag, after `container-image`; the default branch, by hand, allowed to fail; refuses unless `HM_PUBLISH_IMAGE` is `1` | Artifact Hub's metadata for the image's repository, the tree's `platform/packaging/docker/artifacthub-repo.yml`, pushed under the tag `artifacthub.io` and read back, when it holds the repository ID Artifact Hub gave, as it does since 6 October 2026; for a file without one, nothing. Nothing waits for it (`build/ci/container-image-artifacthub.sh`; [ContainerImage.md](ContainerImage.md), "Artifact Hub") |
| `container-image-sbom` | release | linix | a `v*` tag, after `container-image` | Each platform's image's SBOM: Syft over it by its digest, with that architecture's `.deb`'s own SBOM added (`build/ci/container-image-sbom.sh`) |
| `container-image-sign` | release | linix | a `v*` tag, after `container-image-sbom` | The index and each platform's image signed by digest with Sigstore as this pipeline, each image's SBOM attested to it, and all of it verified (`build/ci/container-image-sign.sh`) |
| `container_scanning`, `container_scanning_arm64` | release | linix, in Trivy's image | a `v*` tag, after `container-image` | Trivy over each platform's pushed image, by its digest, with that day's vulnerability database, its report in GitLab's container-scanning format; a finding does not fail the pipeline (`build/ci/container-scan.sh`; [Security scanning](#security-scanning)) |
| `container-image-start`, `container-image-start-arm64` | release | linix | a `v*` tag, after `container-image` | Each platform's pushed image started beside PostgreSQL - the arm64 one under qemu-user - and asked for SMTP and the REST API (`build/ci/container-image-start-test.sh`) |
| `pypi-publish` | release | linix | `master`, started by hand | The release's `hmailserver-cli` to PyPI through a trusted publisher, with attestations; says "not configured" and passes until PyPI has the publisher, and fails once PyPI has the project and no publisher matches ([CommandLinePackages.md](CommandLinePackages.md)) |
| `psgallery-publish` | release | linix | `master`, started by hand | The release's `HmailServer` module to the PowerShell Gallery; says "not configured" and passes until `PSGALLERY_API_KEY` is set, and refuses a module with no Authenticode signature, which the release does not make yet |
| `installer-smoke` | release | linix | a `batch*` branch or a `v*` tag, started by hand | A placeholder. It fails with a sentence saying that the installer smoke test runs from the VMware host ([The installer smoke test](#the-installer-smoke-test)) |
| `renovate` | check | linix | the schedule described `renovate`, and nothing else | Renovate's dependency updates ([Dependency updates](#dependency-updates)) |
| `image-pins` | check | linix | the schedule described `renovate`, and nothing else | Every image the CI files and the build scripts pin by digest, asked of its registry as a runner asks it; a pin the registry no longer serves fails the pipeline, named with every place that names it (`build/check-image-pins.py`; [Image pins](#image-pins)) |
| `forge-export` | check | linix | the pipeline schedule for `master` whose description is `export`, and nothing else | The issues, merge requests, labels, milestones, boards and releases as JSON and the wiki as a git bundle, and GitLab's project export when its token is set: what a push mirror does not carry, kept on linix and as an artifact for Developers only (`.gitlab/ci/forge-export.yml`, `build/ci/forge-export.py`; [Mirroring.md](Mirroring.md)) |
| `scorecard` | check | linix | the schedule described `scorecard`, and nothing else | OpenSSF Scorecard 5.5.0 from cache.nixos.org over the project, its result kept for `package-repos` to publish with a badge; says "not configured" and passes until the owner's `GITLAB_AUTH_TOKEN` exists (`.gitlab/ci/scorecard.yml`, `build/ci/scorecard.sh`; [OpenSSF Scorecard](#openssf-scorecard)) |

`linux-build`, `windows-gate`, `windows-dotnet`, `linux-cross`, `linux-sanitize`
and `linux-sbom` start as soon as the pipeline does. They do not wait for the
checks, because the project's machines are otherwise idle and the Windows gate
is the longest job. `linux-suite`, the two upgrade gates, the two package jobs,
`linux-image` and `linux-repro` wait for `linux-build`, whose artifact holds the
binary and the packages (`linux-image-arm64` waits for `deb-ubuntu26.04-arm64`,
whose artifact holds the arm64 `.deb`), and `release` waits for every job that
can fail a tag's pipeline - the checks, both suites, the upgrade gates, the
package jobs, `linux-cross`, `linux-sanitize`, `linux-image`,
`linux-image-arm64`, `linux-repro`, `windows-dotnet` -
and `windows-installer` ([The release](#the-release)). Each of the two
runners with the tag `linix` runs up to three jobs at once
([The runners](#the-runners)), and the rest queue behind them in the
order GitLab hands them out; bench150's two jobs take the bench's gate lock in turn
([The .NET job](#the-net-job)).

A test report is published for both suites, under the pipeline's *Tests* tab.
The Linux suite's TRX goes through `build/ci/trx-to-junit.py`, and the Windows
suite's NUnit result goes through `build/ci/nunit-to-junit.ps1`. Skipped tests
are expected and do not fail a job: a Linux fixture that the REST API cannot
drive says why and skips, and a Windows test skips where the bench lacks what
it needs (a real `python.exe`, for one). A job passes on "Failed 0" with at least one
test passed. A failed test fails it, and so does a run of no tests. The .NET
tests of `windows-dotnet` are kept as TRX files in its artifact and are not
published as a report: the converter is Python, and bench150's LocalSystem has
none.

Which pipeline runs what
------------------------

| Push or event | Pipeline | Jobs |
|---|---|---|
| `master` | the branch's | all of them but the release jobs, the jobs of a schedule of their own, `installer-smoke`, `sign-off`, `semgrep-sast-gate` and `mr-dependency-check` |
| a `batch*` branch | the branch's | all of them but the release jobs, the jobs of a schedule of their own, `pages`, `package-repos` and `secret_detection`, with `installer-smoke` manual; the change checks among them, against `master` ([Change checks](#change-checks)), and the four security scans on linix unless the batch changes documentation alone from `master`: this is the gate a batch passes before it lands, and the pipeline its merge request is held to |
| any other branch, no open merge request | the branch's, on GitLab's hosted runner | `checkers`, `harnesses`, `editorconfig`, and the four path-bound checks whose files changed |
| any other branch, with an open merge request | the merge request's, on GitLab's hosted runner | `checkers`, `harnesses`, `editorconfig`, the path-bound checks whose files the merge request changes, the change checks `semgrep-sast`, `semgrep-sast-gate` and `mr-dependency-check`, `secret_detection`, and `sign-off` ([Change checks](#change-checks)) |
| a merge request from a protected `batch*` branch of this project | none; the branch's own pipeline stands for it | as for the branch |
| a merge request from `master`, or from another protected branch here that is not `batch*` | the merge request's, beside the branch's own, on GitLab's hosted runner | as for any other branch's merge request: the branch's pipeline runs no change gate |
| a merge request from a fork | the merge request's, run in the fork, with the fork's CI files and on runners the fork's owner chooses | as for any other branch's merge request; its result is not the gate: the change lands through a batch, or on a pipeline a maintainer runs for it here ([Change checks](#change-checks)) |
| a `v*` tag | the tag's | all of them but the jobs of a schedule of their own, `secret_detection`, `sign-off`, `linux-build-gcc`, `semgrep-sast-gate` and `mr-dependency-check` (the commit's `master` pipeline ran `secret_detection`, and its batch `sign-off` and `linux-build-gcc`), with `release`, `release-publish` and `installer-smoke` manual, and `pages` and `package-repos` after `release-publish` ([The release](#the-release)). `semgrep-sast` runs, for the release job to read |
| any other tag | none | |
| the schedule described `fuzz`, on `master` | the schedule's | `linux-fuzz` alone ([The fuzz schedule](#the-fuzz-schedule)) |
| the schedule described `coverage`, on `master` | the schedule's | `linux-coverage` alone ([The coverage schedule](#the-coverage-schedule)) |
| the schedule described `renovate`, on `master` | the schedule's | `renovate`, and `image-pins` beside it ([Dependency updates](#dependency-updates), [Image pins](#image-pins)) |
| the schedule described `security`, on `master`, run once by hand | the schedule's | `secret_detection` alone, over the whole history ([Security scanning](#security-scanning)) |
| the schedule described `export`, on `master` | the schedule's | `forge-export` alone ([Mirroring.md](Mirroring.md)) |
| the schedule described `scorecard`, on `master`, weekly | the schedule's | `scorecard`, then `pages` and `package-repos`, which publish its result ([OpenSSF Scorecard](#openssf-scorecard)) |
| any other pipeline schedule on `master` (the update feed's, `update feed`) | the schedule's | `pages` and `package-repos` alone (see [The update feed](#the-update-feed)) |
| a downstream pipeline on `master` (source `pipeline`: another project's trigger, or the trigger API called with a job token; nothing of this project's starts one) | a downstream pipeline | `pages` and `package-repos` alone |

A schedule is told apart by its description, which GitLab gives the pipeline
as `CI_PIPELINE_SCHEDULE_DESCRIPTION`, compared whole: `fuzz`, `coverage`,
`renovate`, `export` and `security` are the five words that start a job of their own,
which `pages` and `package-repos` refuse, and a schedule
carries no variable. `scorecard` is a sixth, which they do not refuse, on purpose:
that schedule's pipeline deploys the site with the week's score. The project refuses pipeline variables (*Minimum role to
use pipeline variables* is *No one allowed*), and GitLab counts a schedule's
variables as pipeline variables: it will not save a schedule that has one. The
description is a predefined variable, which that setting does not touch, so the
setting stays as it is; allowing variables to Owners instead would let every
Owner start any pipeline, a `v*` tag's included, with variables of their
choosing that override the file's and leave no commit behind.

The four path-bound checks - `chart-and-compose`, `update-helper`,
`package-templates` and `roadmap` - run when a file they check, or
`.gitlab-ci.yml`, changed, as their GitHub workflows did (`rules:changes`). For
a branch's first push and for a tag GitLab counts every file as changed, so they
all run then.

**A push that changes only documentation starts nothing on the project's
machines**, but for the security scans of a batch that carries more (below).
Every job on the project's runners (`.own-runners`, and `db-matrix` with it)
runs only when the push changes a file that is not a Markdown file at the root
of the tree (`README.md`, `CHANGELOG.md`, `Roadmap2.md` and the rest) or under
`.gitlab/` (the issue and merge request templates). Every other Markdown file
counts as code: `hmailserver/docs/*.md` is compiled into the server as its help
pages (`HelpPagesData.cpp`) and read by both suites, and the READMEs further
down are read by tests and packaging. The checks, which name no runner of their
own, still run for such a push. GitLab matches `rules:changes` paths with Ruby's
`File.fnmatch`, which has no "all but", so the rule names what does count:
`**/*[!d]`, `**/*[!m]d` and `**/*[!.]md` (every file whose name does not end in
`.md`, but for the files called `d` and `md`, too short for them, which `**/d`
and `**/md` name) and `[!.]*/**/*.md` (a Markdown file below a directory whose
name does not begin with a dot). `build/check-release-chain.py` matches every
file of the tree against them and fails unless exactly the root's and
`.gitlab/`'s Markdown files are left out. A branch's first push, a tag and a
pipeline started by hand count every file as changed, so the gates always run
for those. The four security scans on a `batch*` branch are the exception: they
compare the branch with `master` (`compare_to: refs/heads/master`), not with the
push before, so a push of documentation on top of code a batch already carries
still runs them, a batch that changes documentation alone from `master` runs
none, and a first push or a pipeline started by hand is diffed in the same way
([Change checks](#change-checks)).

The `workflow:` rules keep this to one pipeline per change. A branch with an
open merge request would otherwise run two pipelines, one for the branch and
one for the merge request. A newer push to the same ref makes an older pipeline
moot: most jobs are `interruptible`, so GitLab cancels the older pipeline when
the project's "auto-cancel redundant pipelines" setting is on, as it is by
default. The two long gates are not: `linux-suite` and `windows-gate` say
`interruptible: false`, and GitLab's default ("conservative") auto-cancel
leaves a pipeline alone once a job that is not interruptible has started. So a
push that lands while a gate runs - three documentation commits after a batch's
gated one, say - does not cut the gate off half-way or leave bench150 in the
middle of a build: the older pipeline runs to its answer and the newer one
queues behind it on the runner. `linux-fuzz`, `renovate` and the release jobs
are not interruptible either, so a push to `master` during the night does not
cut the night's fuzzing short.

Landing a batch runs the full gate twice. The gate runs once on the `batch*`
branch, and again when `master` is fast-forwarded to the same commit. That is
the rule as written ("a push to master runs everything"). A later change could
skip a `master` pipeline for a commit that has already passed on a batch branch.

Protected refs, and why a fork never reaches our machines
---------------------------------------------------------

Each of the project's runners is registered `ref_protected`, with `run_untagged`
off and `locked`. GitLab gives them a job only when all three of these hold:

1. the pipeline is for a protected ref: `master`, a `batch*` branch or a `v*`
   tag, which only the maintainers can push;
2. the job names one of the runner's tags (`linix`, `bench150`,
   `linix-systemd`) - every job does, one that names none by `default:`'s
   `[$HM_RUNNER]`, which is `linix` in such a pipeline alone;
3. the job's `rules:` created it. `.own-runners` in the CI file creates its
   jobs only when `$CI_COMMIT_REF_PROTECTED` is `"true"` and the pipeline is not
   a merge request's, not the fuzz pipeline (which `linux-fuzz`'s own rule
   also holds to a protected ref) and not a scheduled one. For a job that takes
   `default:`'s tags, the workflow's rule that made the pipeline decides instead:
   every rule that makes a merge request's pipeline, or an unprotected ref's,
   sets `HM_RUNNER` to GitLab's hosted runner's tag.

A merge request from a fork runs in the fork's context, on an unprotected ref,
with code nobody here has reviewed. The code could build anything, and the
machines hold things worth protecting: bench150's service runs as LocalSystem,
linix has the lab network, and server2 runs services of the owner's. So a fork's
merge request gets the checks, the change checks and the sign-off check among
them, on GitLab's hosted runners - in the fork, on runners the fork's owner
chooses - and nothing else. The protection is kept in four places on purpose:
the runners, the ref, the rules and the workflow's choice of runner. The rules
and the workflow are the CI files of the commit the pipeline runs, which a
fork's merge request or an unprotected branch writes for itself: against such a
pipeline what holds is the runners' registration, `ref_protected`, since its ref
is not protected. Relaxing one of them is not a reason to relax the others.

The protected refs are project settings (Settings, Repository, Protected
branches and Protected tags): `master`, `batch*` and `v*`, with push and merge
allowed to Maintainers. `master`'s push rule is to become *No one* once the
batch that brings the change checks has landed through them ([Landing on
master](#landing-on-master)).

Every job on the project's own runners
--------------------------------------

Since 9 October 2026 no job of a pipeline for a protected ref that is not a merge
request's runs on GitLab's shared runners. Until then forty jobs named no runner
and ran there, on the namespace's compute minutes: the checks (`checkers`,
`harnesses`, `sign-off`, `editorconfig` and the path-bound four), the change gates
(`semgrep-sast`, `semgrep-sast-gate`, `mr-dependency-check`), `secret_detection`,
`pages` and `package-repos`, `helm-chart-package`, the release's `release`,
`release-sbom`, `release-sign` and `release-publish`, the rehearsal's
`release-rehearsal` and `release-rehearsal-sign`, the container image's jobs in
cosign's and regctl's images and `container_scanning`, the command-line packages'
checks and publishers, the release channels' checks and publishers but
`snap-upload` and `chocolatey-publish`, and `installer-smoke`. The minutes ran out
on 9 October 2026: `batch46`'s pipeline 2931387790 could not start
`semgrep-sast-gate` (`stuck_pending_no_matching_runners`, and once retried
`ci_quota_exceeded`), and a change gate that cannot run holds every landing. The
owner decided that the project's CI depends on no minutes of GitLab's.

**How.** `.gitlab-ci.yml`'s `default:` names the tags `[$HM_RUNNER]`, which every
job that names no runner of its own takes, GitLab's templates' jobs among them
(`semgrep-sast`, `secret_detection`, and any analyzer a template would add).
`container_scanning`, which ran GitLab's template until 10 October 2026, takes them
too. `.gitlab-ci.yml`'s `variables:` set `HM_RUNNER` to `linix`, the tag linix
and server2 carry. The jobs that need a machine of a kind name their own runner as
before (`.linix`'s `linix`, `bench150`, `linix-systemd`). The workflow's rules
choose the runner for each pipeline: every rule that makes a pipeline for a
protected ref that is not a merge request's - `master`, a `batch*` branch, a `v*`
tag, a schedule on `master` - leaves `HM_RUNNER` as it is, and every other rule
that makes one - a merge request's, an unprotected branch's, a `v*` tag's that
has lost its protection - sets it to `saas-linux-small-amd64`, the tag of
GitLab's hosted small Linux runner. GitLab expands a variable in `tags:` as it
creates the job, with the workflow's variables in force. The CI lint API's dry
run of the CI files on 9 October 2026 gave each of the 117 jobs of a `batch46`
pipeline, the 123 of `master`'s and the 103 of `v6.3.6`'s a tag of the project's
(`linix`, `bench150` or `linix-systemd`), and each of the ten of a push to
`renovate/docker`, an unprotected branch, `saas-linux-small-amd64`.

**Why a merge request's pipeline still runs on GitLab's runners.** The project's
runners are registered `ref_protected` (above): GitLab gives them no job of a
pipeline for an unprotected ref, and a merge request's pipeline is one - a fork's
always, Renovate's from its `renovate/*` branches, any from an unprotected branch
here. (The project's setting *protected merge request pipelines* makes one
protected only when both its branches are and the person may merge to the target;
the workflow sends that one to GitLab's runners too, as `.own-runners` keeps
every merge request off the project's machines.) A job tagged `linix` there would
wait for a runner until GitLab failed it, so it runs on GitLab's hosted runner. A
fork's pipeline runs in the fork, on the fork's own minutes. Renovate's merge
requests, a pipeline a maintainer runs for a fork's merge request here and an
unprotected branch's pipeline count against the project's, and while those are
spent they fail with `ci_quota_exceeded`: none of them is what lands a change,
which goes through a `batch*` branch, whose pipeline runs on the project's
runners alone, and a Renovate merge request is folded into a batch ([Dependency
updates](#dependency-updates)). Taking a merge request's pipeline onto the
project's machines is the owner's decision: it needs a runner registered for
unprotected refs, which would run code nobody here has reviewed (above), or
Renovate's branches protected and a rule of its own.

A fork's own protected branch - its `master`, in the fork's project - takes
`linix` too, and no runner of the fork's carries it: that pipeline's jobs wait
until GitLab fails them, as the jobs `.own-runners` makes there always have. A
fork's feature branch and its merge request run on GitLab's hosted runner, on
the fork's own minutes, and those are what a contributor's change is shown by.

**What holds it.** `build/check-release-chain.py`, which `checkers` runs, fails
when `default:` names other tags; when `variables:` set `HM_RUNNER` to anything
but `linix`, when anything else in the CI files sets it but a workflow rule, or
when a job says `inherit:`; when the workflow's rules, tried for every kind of
pipeline (its ref protected or not, every `CI_PIPELINE_SOURCE`) and every way
their other clauses can go, make a pipeline of a protected ref that is not a
merge request's with `HM_RUNNER` anything but a tag of the project's, or any
other pipeline with anything but `saas-linux-small-amd64`; and when a job that
names its own tags names none, a variable other than `HM_RUNNER`, tags no runner
of the project's carries, or none of `linix`, `linix-systemd` and `bench150` (a
runner anywhere may carry `linux` or `docker`) - or names the project's and has
rules that make it in a merge request's or an unprotected ref's pipeline, where
it would wait for a runner that never takes it. It reads every CI file, the
included ones too; its self-test breaks each of those in a copy, in
`.gitlab-ci.yml` and in an included file, and each break is caught. GitLab's
templates are not in the tree, so a job one of them adds is read only where a CI
file here names it; any other takes `default:`'s tags unless its template names
tags of its own, which none of the three included does: the CI lint API's dry
run above lists every job a pipeline makes, the templates' with the rest, each
with its tags. `build/ci/repo-hygiene.py`'s Token-Permissions reads `default:`'s
tags as well, and judges a job that takes `[$HM_RUNNER]` by the workflow's rules
([The repository
checks](#the-repository-checks-and-the-scorecard-checks-left-out)).

**What changes.** linix and server2 take the forty jobs, each short ([How long
each job takes](#how-long-each-job-takes)), in images pulled on first use and
kept ([linix](#linix)), and the cache `package-repos` keeps the release's
package files in is the runner's own, in the volumes that hold `linux-fuzz`'s,
not GitLab's: a run that finds none there downloads every file again, each held
to `releases.json`'s size and SHA-256 as before. A project CI/CD variable called
`HM_RUNNER` would stand above the file's, as any project variable stands above a
job's own: none is made. Fulcio's certificate of each signature `release-sign`,
`release-rehearsal-sign` and `container-image-sign` make names the runner
self-hosted (its Runner Environment extension, 1.3.6.1.4.1.57264.1.11, said
`gitlab-hosted` before); neither `SigstoreVerifier.cpp` nor a script of the
release reads that extension, so a server installs such a release as before.
`semgrep-sast`, the heaviest of the forty, was run over `batch46` on 9 October
2026 in its image as each runner gives a job: with linix's limits (six CPUs and
8 GB, the VM's sixteen processors in view) it took 188 s, with server2's (28
CPUs and 40 GB) 254 s, and each found the same 338 findings, with no rule giving
up on a file.

**What it trusts.** The ID tokens of the jobs that sign - `release-sign`'s and
`container-image-sign`'s `SIGSTORE_ID_TOKEN` in a `v*` tag's pipeline, and
`pypi-publish`'s - are held on linix or server2 while those jobs run, where
until 9 October 2026 a VM of GitLab's held them, made for the one job. Whoever
is root on either machine or in its `docker` group, or a job of `linix-systemd`,
which runs privileged on linix's Docker, in a protected ref's pipeline at the
same time, can read them while the job runs, and with a tag's
`SIGSTORE_ID_TOKEN` have Fulcio certify a key as that release's pipeline while
the token is valid. The two machines were trusted with the release already -
they build the Linux packages it carries - and only a protected ref's pipeline
reaches them. Nothing else is handed to them that was not: every job of a
protected ref's pipeline is given every protected variable, on these machines as
before; what is new on them is the tag's signing identity.

Change checks
-------------

Every change reaches `master` through a merge request, and GitLab merges one
only on a pipeline that succeeded (*Pipelines must succeed*). Which pipeline that
is depends on where the change comes from. A merge request from Renovate or from
any branch of the project that is not a protected `batch*` one runs a pipeline of
its own, which never reaches the project's runners (above): what it must pass
runs on GitLab's hosted runners, holds no secret, and says in its own log what
held it. A maintainer's `batch*` branch is protected, and its merge request runs
no pipeline of its own (the `workflow:` rules), so GitLab holds that merge
request to the branch's pipeline for the same commit. A merge request from a
fork runs a pipeline of its own too - the first workflow rule compares the
merge request's source project with its own project, not with
`CI_PROJECT_ID`, which in a fork's pipeline is the fork's, so a fork's protected
branch is no `batch*` branch here - but that pipeline runs in the fork, with the
fork's CI files, on runners the fork's owner can choose: what it says is the
fork's to report. Such a change lands through a batch, whose pipeline runs here,
or on a pipeline a maintainer runs for the merge request in this project after
reading its CI files; `build/land-via-mr.py` refuses a pipeline another project
ran. The change checks run in each - on linix or server2 in a batch's pipeline,
on GitLab's hosted runner in a merge request's - (`.gitlab/ci/mr-checks.yml`,
`.gitlab/ci/gitlab-security.yml` and one checker),
and they hold the policy of [SECURITY.md](../../SECURITY.md), "Vulnerability
management policy", "Every change". Beside the checks every pipeline runs, three
hold a change on what it brings:

- **Every committed binary is in its manifest.** `build/check-binary-manifest.py`,
  which `checkers` runs as it runs every `build/check-*.py`, reads every file git
  tracks. It fails on a binary that `hmailserver/docs/third-party-binaries.json`
  does not list - a binary by OpenSSF Scorecard's list of extensions, or by a PE,
  ELF, OLE, Mach-O, Java class, WebAssembly, ar (a static library or a `.deb`),
  RPM, DEX or Chrome extension header whatever the file is called;
  on a file the manifest marks as fetched, gathered, generated, downloaded or
  Windows' own that is committed after all; and on a committed one whose bytes are
  not the SHA-256 its entry records ([ThirdPartyBinaries.md](ThirdPartyBinaries.md)).
  What the GitHub workflow `verify-binary-provenance.yml` did until 18 September
  2026; its Windows half, the build inputs' Authenticode signatures, is the bench150
  job `build-inputs-authenticode` from 6.3.6. On 28 September 2026 it read 7,084 tracked files and found no binary; a PE
  image named `.bin`, added to the index, failed it.
- **`semgrep-sast-gate`** reads `semgrep-sast`'s report with
  `build/ci/security-gate.py --merge-request auto`, the release's own rule and
  code ([Before a release](#before-a-release-the-scans-hold-it)): a finding of
  severity Critical or High holds the change unless
  `.gitlab/security-exceptions.yml` accepts its fingerprint. Two things differ
  from the release's gate. The exceptions are the change's base's, never its
  own copy for a finding in a file it touches, so a change cannot accept its
  own finding: an exception lands first, by a change of its own - which changes
  no source file and so holds nothing here - and the change after it. And only
  the findings in the files the change touches hold it - the files it adds,
  alters or removes, and both ends of a move: a change answers for what it
  touches, while a finding already on the target branch, in a file the change
  never opened, holds the next release instead. When the change alters the scan
  itself - edits, removes or moves `.gitlab/ci/gitlab-security.yml` (the
  analyzer, its pin, its options), `.gitlab/sast-ruleset.toml` (GitLab's custom
  ruleset, of which none is committed), `build/ci/security-gate.py` or
  `build/ci/security-findings.py` - or what reaches it or runs the gate -
  `.gitlab-ci.yml`, whose variables for every job reach the analyzer as the
  security file's own do, and `.gitlab/ci/mr-checks.yml` - every finding in the
  tree counts. When the change re-pins the analyzer (`semgrep-sast`'s image), and
  only then, a finding in a file the change does not touch may be accepted by
  the change's own copy of the exceptions file as well as by the base's, and one
  in a file it touches still by the base's alone: a change that re-pins an
  analyzer GitLab's registry no longer serves, while `master` holds findings
  nobody has accepted, can carry their exceptions with it, where no change of
  their own could land first, since the scan that would judge it is the one
  gone. Any other change of the scan's configuration answers for the whole tree
  by the base's exceptions alone, and a copy of its own that cannot be read
  accepts nothing. The findings outside the changed files
  are counted in the log, never described: the log of a change's job is public,
  and those are the tree's unaccepted findings, which the release's gate holds
  and a maintainer reads in the scan's own artifact (until 6.3.6 the first
  twenty were printed).

  The report must be the one `semgrep-sast` wrote in this pipeline. A report
  that is missing, or whose scan did not finish, holds the change; so does one
  whose `scan.analyzer.id` is not `semgrep`, GitLab's analyzer, and one that
  began more than five minutes before the pipeline was created
  (`CI_PIPELINE_CREATED_AT`, five minutes for a runner's clock). And a tree that
  carries `gl-sast-report.json`, `semgrep.sarif` or anything under `security/`
  as a tracked file holds the change whatever the report says: a committed copy
  would stand in for one the scan did not write, as a local run of the analyzer
  followed by `git add -A` would leave one. `semgrep-sast` itself is not
  `allow_failure`, where GitLab's template makes it so: a scan that fails, or an
  image that cannot be pulled, fails the pipeline, and `semgrep-sast-gate`, which
  needs it, does not run on what it did not write. It removes
  `gl-sast-report.json` and `semgrep.sarif` before `/analyzer run`, and
  `.gitignore` keeps the scans' outputs out of a `git add`.

  Whole-tree mode widens what counts; it cannot see a finding the change keeps
  out of the report. So a change that **narrows what a scan reads** is a
  declaration, as an exception is, and lands alone: it is held while it changes
  anything else a scan reads - anything but a Markdown document, the CI files,
  the files that say what the scans read, the baselines, the exceptions file, the
  VEX document and the three generated documents. A narrowing is a line gained
  by a `.semgrepignore` at any depth (GitLab's analyzer renames the root's to
  `.semgrepignore.local` and includes it in its own template; Semgrep reads one in
  a directory too), by `build/ci/semgrepignore` or by
  `build/ci/cppcheck-suppressions.txt`; in Semgrep's lists, which take
  `.gitignore`'s syntax, a `!` line lost, or the lines put in another order while
  one has a `!` line, since a `!` line takes back only what a line before it left
  out (a `!` line gained, or any other line lost, leaves out less); any change of
  `.gitlab/sast-ruleset.toml` or of `build/ci/clang-tidy.yaml`; a line gained,
  lost or moved between the `# scan-scope begin` and `# scan-scope end` marks of
  `build/ci/static-analysis.sh` and `build/ci/semgrep.sh` - the lines that choose
  the files cppcheck, clang-tidy and Semgrep are given, their options, the
  checks, the rule packs and the rules fetched for each, and what of a run that
  failed, or of a file clang-tidy did not finish, stands as the report - or one
  of those tools' options (`--exclude`,
  `--suppress`, `-i`, `--checks` and the like) put outside them, or marks that do
  not pair (such a change is the narrowing's own file, not a file it changes
  besides, while the script changes nowhere else); an entry of
  `SAST_EXCLUDED_PATHS` or
  `SAST_SEMGREP_EXCLUDED_PATHS`, in any CI file, that no entry of the base's
  covers (`/tests` under `tests` covers nothing new, `src/x` under `src` neither);
  a value of either that cannot be read on its line; and any change of any other
  `SAST_*`, `SEMGREP_*` or `SECURE_*` variable, or of `HM_TIDY_CHECKS`, in any CI
  file. `static-analysis.sh` refuses `HM_TIDY_CHECKS` in a GitLab job, so that a
  project variable cannot choose its checks either, and `semgrep.sh` refuses to
  run without `build/ci/semgrepignore`, without which Semgrep's own default would
  leave out `build/` and every test directory. The analyzer passes both lists to
  Semgrep as `--exclude`, which reads a
  pattern with no slash as a name at any depth - seen on 6 October 2026 with the
  analyzer's Semgrep 1.174: `tests` left out `a/tests/t.py`, `/tests` only
  `tests/t.py` - so each of `semgrep-sast`'s own entries is held to the root, the
  Debian package's autopkgtests named where they are, and the job sets
  `SAST_SEMGREP_EXCLUDED_PATHS` to nothing itself, so that no value a CI file
  gives every job reaches it.

  And whatever the scan found, it holds a change that silences a finding without
  a declaration on the base: one that adds an entry to
  `build/ci/static-analysis-baseline.txt` or `build/ci/semgrep-baseline.txt`, or
  takes an entry's reason away - a change may delete an entry, or give one its
  reason, and nothing else - and one that raises, in any file a scan reads, the
  count of the comments by which a tool passes over a finding: Semgrep's
  `nosemgrep` (in any case, and its short form `nosem`, which Semgrep takes as
  its marker whatever follows it after a blank, as it does `# nosemaphore`),
  clang-tidy's `NOLINT`
  in each form that begins a suppression - of its line, of the next line, or of
  a block, not the block's end - and cppcheck's `cppcheck-suppress` with its
  `-begin`, `-file` and `-macro` forms and its bracketed list (not `-end`). This
  page names the forms no more closely, since the server's help pages are made
  from it and clang-tidy looks for its directives in a file's text, strings
  included. The comments of each file the change adds or alters are counted at
  the base and at the head, so an edited line keeps the comment it had, and a
  file git sees renamed keeps the count it had under its old name; a source file
  is counted whatever bytes it holds, a NUL among them, which the compiler and the
  scans read past. The Markdown
  files, the two baselines, the exceptions file and the three files generated
  from documents and page text that every scan leaves out (`HelpPagesData.cpp`,
  `PortalPageData.cpp` and `PortalLanguagesData.cpp`) are not counted, since
  their words about the comments silence nothing. A file of the gate's own that
  names the comments spells them in pieces, as `security-gate.py` does, or it
  would hold the change that edits it. It holds a source file moved from where
  `semgrep-sast` reads it to where it does not - a move as git pairs it, or a
  source file deleted while one of its name is added where the scan reads
  nothing, which is a move rewritten past the half git needs to pair it (a new
  test named like a source file the same change deletes is held with it: it lands
  apart, or under a name of its own; git's similarity threshold is not lowered
  instead, since at a low one the licence header every file begins with can pair
  two short files that are not one) - and one added or changed where
  only the analyzer's own template leaves it unread - a `build/`, `dist/`,
  `vendor/` or `node_modules/` directory below the root, a minified script - which
  no exclusion of the project's names; `build/` at the root is the one such place
  said and read elsewhere (by `semgrep`, in a batch). And it holds a source file
  the change adds or alters that one of Semgrep's rules gave up on - a time limit,
  or the memory or stack it ran out of - as the notifications of
  `semgrep.sarif`, which the analyzer writes beside its report and the job keeps,
  say: the analyzer fails a scan on an error-level notification only, and these
  are warnings. Where Semgrep's parser stumbled, which it reads on past, the
  notification is counted in the log and holds nothing: over this batch on 6
  October 2026, 87 of the C++ and C# files drew one (`_T` strings run together,
  `WINAPI`, a class declared over two lines), 18 of them with findings all the
  same, and three of the Helm chart's templates. Those kinds, and the file each
  notification names, are Semgrep's words, read from analyzer 6.26.1 (Semgrep
  1.174.0) on 6 and 7 October 2026 and kept word for word in the gate's
  `SARIF_FIXTURE`: a notification of a kind the gate does not know, a time limit
  that names no file of the checkout as the analyzer names one - from the root,
  with no `./`, no absolute path and no `file:` URI, any of which would match no
  changed file - or a SARIF with no run stops the gate as an input it cannot read
  (exit 2), never a pass, so that a new analyzer's words cannot pass a hole
  unseen ("Re-pinning `semgrep-sast`'s analyzer", below).

  When the change alters the gates' own code - `security-gate.py`,
  `security-findings.py`, `mr-dependency-check.py`, `dependency-scan.py` or
  `osv-offline-db.py` - the base's copy of each gate judges it too: the gate
  makes a worktree of the change's head, puts the base's copies of those five
  files in it, runs the base's copy with the base it found, and passes the change
  only when both copies pass it (a hold from either holds it). So an edit of the
  fingerprints, or a library left out of `dependency-scan`'s list, does not judge
  the change that makes it. Whether the base's copy runs is read from the code
  that runs - the checkout's five files against the base's, line endings aside -
  and from no variable: in its worktree the base's copy finds its files the
  base's and runs none of its own (one that found them otherwise would stop
  there, unable to check, exit 2), and nothing a CI file or a project sets stops
  it. Each gate says in its log "The base's copy of <script> exited <code>.", and
  `build/land-via-mr.py` lands a batch that changes those files only when both
  gates' logs say it exited 0 (or that the base has no copy of the script), so
  that a change of the code that decides whether the copy runs shows. No file
  beside a gate script runs as its code either: Python puts a script's own
  directory first on its module path, so a `build/ci/json.py` a change added
  would stand in for the `json` the gate imports, in a change that leaves every
  gate file alone; each script of the gates and of the landing takes its own
  directory off that path before it imports anything else, and
  `build/check-release-chain.py` holds each to the same line (`PATH_GUARD`). The
  CI files still say what runs: this stands against a mistake in the code, not
  against a change that edits the jobs, which a person reads
  ([Landing on master](#landing-on-master), `--gates-read`).

  This is OpenSSF Baseline OSPS-VM-06.02: a change with a weakness above the
  threshold is blocked unless it is declared not exploitable. On 28 September
  2026 GitLab's analyzer, run in the job's own image on two CPUs with the job's
  options over 6.3.6's batch with this change in it, found 308 findings, 193 High
  and none Critical, none in the change's own files; asked as the gate asks for
  this change, which touches the scan and so answers for the whole tree, every
  High finding was accepted by the exceptions file and nothing held it. With a
  sample Critical finding added to a report, the gate over a two-commit change
  that leaves the scan alone held it in a file the change touches (exit 1), and
  not in a file it does not. On 6 October 2026 the gate was run as the job runs it
  over the batch that brought the baseline and comment rules, against `master` at
  `a5a02b3be` and `master`'s own `semgrep-sast` report: it alters five of the
  scan's files, so every finding counted, and of 335, all 224 High ones were
  accepted by `master`'s exceptions; both baselines kept their 125 and 691
  entries, and no file it touches had more of the comments than at the base. It
  passed (exit 0). Each of twenty-one deliberate breakings of the gate's rules for
  a change, made in a copy of the script, failed its self-test.
- **`mr-dependency-check`** reads every change, with no list of files that start
  it: when the change adds no version and moves no pin it says so and ends,
  before any download, so no kind of file can slip past a list of triggers.
  `build/ci/mr-dependency-check.py` lists every dependency version at the head
  that the base has nowhere - in a `packages*.lock.json` (the ARM64
  `packages.arm64.lock.json` beside each project among them), a `packages.config`
  or a `*requirements*.txt` (every name Renovate's `pip_requirements` manager
  updates, `build/ci/cli-packages-requirements.txt` included) - and looks each one
  up in OSV's own export of its ecosystem's advisories,
  `https://osv-vulnerabilities.storage.googleapis.com/<ecosystem>/all.zip` (NuGet's
  2.5 MB and PyPI's 35 MB on 28 September 2026), the export
  `build/ci/osv-offline-db.py` builds Renovate's database from. An advisory
  affects a version when its `versions` list names it or one of its ECOSYSTEM or
  SEMVER ranges holds it, compared as NuGet and PEP 440 order versions; a
  withdrawn one affects nothing. A malicious package (OSV's `MAL-` records, the
  OpenSSF malicious-packages data) is Critical. Any other advisory's severity is
  the GitHub advisory database's rating where OSV carries it, else the CVSS 3.x
  score of its vector, else the CVSS 4.0 score of its vector (since 6.3.6,
  FIRST's calculator's method, the same as Red Hat's `cvss` 3.6 on every base
  vector with each Exploit Maturity and on 300,000 random ones with environmental
  metrics), else the highest of its aliases'; one with no rating anywhere is
  listed and holds nothing, as at a release. An advisory it cannot decide - the
  added version, or a version bounding one of the advisory's ranges, is one it
  cannot order as the ecosystem does, and no `versions` list names the added one -
  is not passed over: Critical or High, it holds the change as one that MAY
  affect the version (since 6.3.6; until then it passed). A vulnerability OSV
  carries under several ids (a GHSA and its PYSEC) is said once, by the record
  that rates it highest. A requirements line the change adds that pins no
  version by `name==version` - a range, `name @ <url>`, a path, an editable,
  another index or a page of links, an option of pip's it does not know, a
  `${...}` that pip takes from the environment - holds the change too, since pip
  would install what OSV cannot be asked about; its hashes, `--require-hashes`,
  PyPI's own index and a requirements file of the tree, of a name it reads
  itself, are allowed, and `-r` or `-c` naming one the tree does not hold (a URL,
  an absolute path, a file outside the checkout) holds the change. On 28 September 2026 a fixture commit adding `urllib3`
  1.26.4 and NuGet's `System.Text.Json` 8.0.0 was held on six High advisories of
  the first and two of the second, one that changed a document passed without
  reading OSV, and one that added `Humanizer.Core` 2.14.1 passed with no advisory.

  It also asks about every native or vendored library whose pinned version the
  change moves, read as `dependency-scan` reads them: OpenSSL, Boost and libpq
  from `hMailServer.vcxproj`'s paths, zlib from `zlib.h`, QuickJS from its
  `VERSION`, and MariaDB Connector/C, 7-Zip and SQL Server Compact from
  `third-party-binaries.json` - every version a file pins, so that a path for
  ARM64 alone counts - and the components of a committed
  `native-dependencies*.json`. Each new version is asked of `api.osv.dev` by its
  upstream repository's tag, with the library's control version beside it, as
  `dependency-scan` asks, and rated as above, an alias's record fetched when the
  advisory has no rating of its own. A version older than its repository's first
  tag (7-Zip's before 22.01), which OSV cannot match, is held by that tag's
  Critical and High advisories, as ones that may affect it; so is a version older
  than its control that OSV names no advisory for while it names the control's,
  by the control's (OpenSSL's 0.9.6 draws none, its 4.0.0 43). A control that
  OSV is known to name - OpenSSL's, Boost's, zlib's, Connector/C's and 7-Zip's,
  each of which drew advisories on 6 October 2026 (`control_draws` in
  `dependency-scan`'s list) - and that draws none is OSV's data not read: exit 2,
  as `dependency-scan` fails its scan for it. A library OSV has no usable data
  for - no repository it knows (SQL Server Compact), or nothing for a control OSV
  is not known to name either (libpq, QuickJS) - is listed as UNKNOWN and holds
  nothing; it is read by hand against the SBOM at release. Its self-test holds a
  change that sets zlib's version to 1.2.11 on CVE-2022-37434, Critical, as OSV
  answers for that tag.

  A Critical or High advisory holds the change unless one of two declarations,
  as the base has them, accepts it: an entry of `.gitlab/security-exceptions.yml`
  of scan `dependency-scan` that names the advisory, or one of its aliases, and
  the package (a native library by the name `dependency-scan` reports, `zlib` or
  `libpq (PostgreSQL client)`); or a `not_affected` statement of the VEX document,
  `.gitlab/hmailserver.openvex.json`, that names the advisory and the package at
  that version by its purl - `pkg:nuget/` (names compared without case),
  `pkg:pypi/` (PEP 503's names) or `pkg:generic/<name>` for a native library. A
  statement for another version, and the change's own copy of either file,
  accept nothing; a base VEX document that cannot be read dismisses nothing, and
  says so. Lock files, pins or advisories it cannot read are never a pass (exit
  2): the job is retried. A base whose own files are broken holds no change by
  that alone, or no change could mend them: a base lock file it cannot read
  names nothing, so every version the head's copy names is looked up, and a base
  exceptions file it cannot read, or an entry of it reviewed after today, accepts
  nothing, and is said (the same holds in `semgrep-sast-gate`). OSV's export is
  fetched only when the change adds a NuGet or PyPI version, and `api.osv.dev`
  asked only when it moves a native pin. What GitHub's dependency review did
  until 18 September 2026; OpenSSF Baseline OSPS-VM-05.03.

**The base.** Both gates take it from `change_base()` in
`build/ci/security-gate.py` ("auto"). In a merge request's pipeline
(`CI_PIPELINE_SOURCE` `merge_request_event`) it is
`CI_MERGE_REQUEST_DIFF_BASE_SHA`, the commit of the target branch the merge
request's diff starts from, taken only when it is an ancestor of `HEAD` and of
the target branch fetched by name, so that a base off the target branch - a
commit of the change's own, say - is refused (exit 2); a merge request's
pipeline without it exits 2, since a merge request may target another branch
than `master`, and its base is GitLab's to give. The variable in a pipeline of
another kind exits 2 too. In a `batch*` branch's pipeline it refuses a shallow
clone, fetches `master` by name (`git fetch --no-tags origin
+refs/heads/master:refs/remotes/origin/master`), and takes the merge base of
`HEAD` and that, which is `master`'s tip whenever the batch is rebased onto it,
as the landing requires; the log says so: "The base: <commit>, where HEAD leaves
master ...", and `build/land-via-mr.py` reads that line back from each gate's log
and refuses a landing whose gates name another. GitLab lets a variable a CI file
defines stand in for one of its own, these among them: a CI file of the change
that defines a `CI_*` or `GITLAB_*` variable holds it (exit 1), and
`build/check-release-chain.py` refuses one in every pipeline (but
`CI_DEBUG_SERVICES`, which `container-image.yml` sets for a service's log). The
two jobs clone with the history (`GIT_DEPTH 0`), as `sign-off` does, and each
runs its checker's self-test before the check.

**When they run.** In a merge request's pipeline, and in a protected `batch*`
branch's pipeline that is not a schedule's or a downstream one's
(`.mr-check`'s rules); `semgrep-sast` in exactly those, for `semgrep-sast-gate`
to read, and for `master` and a protected `v*` tag besides, for the release job.
The workflow's first rule leaves a merge request without a pipeline of its own
for exactly the branches the gates run for in a branch's pipeline - protected
`batch*` branches of this project - and `build/check-release-chain.py` holds the
two to each other, so that protecting another pattern of branches later opens no
route without a gate. Neither gate is `allow_failure`, nor is `semgrep-sast`,
neither has a `rules:changes` list, and no variable turns either off: GitLab's
SAST template skips `semgrep-sast` when `SAST_DISABLED` is set, and the rules of
none of the three read it, since a project variable must not take a blocking
gate away unseen. Nor does a variable a CI file sets narrow `semgrep-sast`
unseen: it sets `SAST_SEMGREP_EXCLUDED_PATHS` to nothing itself, a change of any
other variable that narrows it lands alone (above), and
`build/check-release-chain.py` refuses `SAST_SEMGREP_EXCLUDED_PATHS`,
`SAST_RULESET_GIT_REFERENCE`, `SAST_EXCLUDED_ANALYZERS` and `HM_TIDY_CHECKS` in
any CI file but that one value. A project variable, which only the owner can
set, stands above a job's own ("What they cannot hold", below). Each keeps
GitHub out of the job itself
([Nothing from GitHub, in the job](#nothing-from-github-in-the-job)).

**What fails them that is not their answer.** An input that cannot be read - the
base, `master`'s fetch, a lock file, a pin, OSV's export or `api.osv.dev` - is
exit 2, and the job is tried twice more (`retry: exit_codes`, GitLab 17.5 and
later); so is `apt-get`'s exit 100, when Debian's mirrors cannot be reached for
git. A hold is exit 1, which is never retried, and nor is a self-test that
fails. All three jobs are also tried twice more on a failure of GitLab's own,
the seven reasons of `.gate-retry` in `.gitlab/ci/gitlab-security.yml`: a runner
or the scheduler that failed, a registry or the network that could not be
reached, a job left pending while runners were free, or one that stopped
reporting. Not `runner_configuration_error`, which an image the registry no
longer serves gives, and which no retry mends: that is what the next paragraph
is for. `semgrep-sast`'s rules have a time limit for each file
(`--timeout=120`), and a slower runner can reach it where another did not: on 6
October 2026 the analyzer over this batch on server2 found 339 findings where
`master`'s shared-runner report had 335, the four more Medium ones in
`Portal.js`, a file the batch does not touch. A file the change touches that a
rule gave up on holds it (above); one it does not touch can lose a finding that
way, which then holds the release when a later scan finds it again.

**`semgrep-sast`'s image is the project's own copy.** GitLab's registry does not
keep serving a digest its tag has moved on from: on 5 October 2026 the digest
pinned on 26 September answered "manifest unknown", and `semgrep-sast` failed
with `runner_configuration_error`. A gate that holds every change cannot stop
for that, so the job runs
`registry.gitlab.com/hmailserver/hmailserver/mirror/semgrep:6.26.1-b98bc4dd3949`,
named by the same digest GitLab's index has, so the bytes
are GitLab's. `bash build/ci/mirror-semgrep.sh <version> [sha256:<digest>]` makes the
copy with skopeo (`copy --all --preserve-digests`: the index, both platforms'
images and their attestations), reads it back, and prints the line for
`gitlab-security.yml`. The tag ends in the digest's first twelve hex digits, so a
later rebuild under the same version is a tag of its own and never moves the one
a commit names, and the registry's cleanup policy keeps it by name (on 6 October
2026 the policy is switched off, and removes nothing).
The analyzer
is MIT-licensed, GitLab's rules in it too, and the Semgrep it runs LGPL-2.1 with
public sources; the copy is the image unmodified, for this project's CI (the
script's header says more). Renovate does not look up the project's own
registry, so a new analyzer digest is taken by hand, before a release tag.

The script handles no token, as `build/ci/build-linux-image.sh` does not: it
needs a `docker login registry.gitlab.com` made before it runs, by a person
whose token may write the project's registry, and hands the file that login
wrote to skopeo's container, read-only and for the copy's destination alone. It
refuses a login kept in a credential helper, which the container cannot reach,
and one that has no entry for the registry. Best in a directory of its own,
removed after, the script started with `bash`, as every script of the tree is,
since it is not executable in a checkout:

```
export DOCKER_CONFIG="$(mktemp -d)"
docker login registry.gitlab.com
bash build/ci/mirror-semgrep.sh 6.26.1 sha256:<digest>
docker logout registry.gitlab.com
rm -rf "$DOCKER_CONFIG"; unset DOCKER_CONFIG
```

`bash build/ci/mirror-tumbleweed.sh`, which copies openSUSE Tumbleweed's images for
the RPM jobs ([PackagesRpm.md](PackagesRpm.md)), takes the same login in the
same way.

**`secret_detection`'s image is the project's own copy too.** The digest of
GitLab's `security-products/secrets:7` pinned on 5 October 2026 answered
"manifest unknown" by 7 October, and `secret_detection` failed with
`runner_configuration_error` in `master`'s pipelines of 7 and 9 October
(2923938530, 2930660579) before it ran; `build/scan-secrets.py`, which takes its
gitleaks from the image the job names, could then run only on linix, where an
earlier pull had left the image cached. The job now runs
`registry.gitlab.com/hmailserver/hmailserver/mirror/secrets:7.40.0-1f54871d97a7`,
named by the digest GitLab's index for 7.40.0 had on 9 October 2026, which
`bash build/ci/mirror-secrets.sh <version> [sha256:<digest>]` copies as
`mirror-semgrep.sh` copies its analyzer, with the same login. The image the
pin had named could not be copied instead: its index is GitLab's no longer, and
linix's cache holds its linux/amd64 half alone. The new digest is the same
analyzer rebuilt: in its linux/amd64 image the analyzer, gitleaks 8.30.1 and
GitLab's rules 0.25.1 (`/gitleaks.toml`) are byte for byte the old image's, and
of its Alpine packages only zlib moved, from 1.3.2-r0 to 1.3.2-r1. gitleaks in
each image, run as `scan-secrets.py` runs it over `master`'s whole history
(1,808 commits), found nothing with `.gitleaksignore` and the same 40 findings
without it. The analyzer is MIT-licensed, GitLab's rules in it MIT Expat and
gitleaks MIT; the copy is the image unmodified, for this project's CI (the
script's header says more). Renovate does not look up the project's own
registry, so a new digest is taken by hand, as `semgrep-sast`'s is. The job
names it on one `image:` line, the line `scan-secrets.py` reads.

**Re-pinning `secret_detection`'s analyzer.** A new analyzer can bring a new
gitleaks, and `scan-secrets.py` reads gitleaks' own words: a scan whose `git log`
failed is told from a clean one by the `ERR` lines gitleaks 8.30.1 logs before it
writes an empty report and exits 0 (`findings_of`, and the log its self-test
holds). Before the pin moves to an image whose `SCANNER_VERSION` is another,
`scan-secrets.py` is run over a range whose base the repository does not have,
and what the new gitleaks logs read; and the whole history is scanned in both
images and the two reports compared. `mirror-secrets.sh`'s header says the same.
New rules (`SECRET_DETECTION_RULES_VERSION`) may change GitLab's global
allowlist: then `build/ci/secret-detection-rules.sh` refuses, and both scans
with it, until the script is brought up to date with GitLab's new table - the
two entries it changes, and `GITLAB_TABLE`, the new table's hash ([What the
secret scans read](#what-the-secret-scans-read)).

**Re-pinning `semgrep-sast`'s analyzer.** A new analyzer is a new Semgrep, and
`semgrep-sast-gate` reads Semgrep's own words in `semgrep.sarif`: the kind of
each notification and the file its text names (`security-gate.py`'s
`sarif_holes`, `SARIF_GAVE_UP` and `SARIF_PARSE`). Before the pin in
`gitlab-security.yml` moves, those words are read again from the new image, as
they were from 6.26.1 on 7 October 2026: its `semgrep` run with `--timeout 1`
and `--sarif` over a Python file made to take a rule past that (a function of
some thousands of assignments, and a taint rule from `+` to `os.system`), and
over a C++ file its parser stumbles in (`WINAPI`); each
`toolExecutionNotifications` entry's descriptor id and message text compared
with the gate's `SARIF_FIXTURE`; a new kind put in its list; and
`SARIF_READ_FOR` set to the new version. Until it is, `security-gate.py`'s
self-test fails, and `checkers` with it; and a notification of a kind the gate
does not know stops `semgrep-sast-gate` (exit 2) rather than passing a file the
scan gave up on. `mirror-semgrep.sh`'s header says the same.

**What holds the jobs to this.** `build/check-release-chain.py`, which
`checkers` runs in every pipeline, fails when `secret_detection` runs any image
but the project's copy, named by its digest on one `image:` line, with the
digest's first twelve hex digits in its tag, or does not put the rules
`build/ci/secret-detection-rules.sh` makes in place of `/gitleaks.toml` right
before the analyzer, when the tree carries a `.gitlab/secret-detection-ruleset.toml`,
or when `build/land-via-mr.py`'s `GATE_FILES` leave the script out; when `semgrep-sast` has no rule for a
protected `batch*` branch, reads `SAST_DISABLED`, runs any image but the
project's copy named by its digest, does not take `.gate-retry`, is
`allow_failure`, does not remove `gl-sast-report.json` and `semgrep.sarif`
before `/analyzer run` or keep both, has an entry of `SAST_EXCLUDED_PATHS` not
held to the root, or does not set `SAST_SEMGREP_EXCLUDED_PATHS` to nothing; when
a CI file sets `SAST_SEMGREP_EXCLUDED_PATHS`, `SAST_RULESET_GIT_REFERENCE`,
`SAST_EXCLUDED_ANALYZERS` or `HM_TIDY_CHECKS`, or `SAST_EXCLUDED_PATHS` outside
`gitlab-security.yml`, or defines a `CI_*` or `GITLAB_*` variable; when the tree
carries a `.semgrepignore`, `.gitlab/sast-ruleset.toml` or a scan's report; when
the workflow's first rule is not the one for a protected `batch*` branch of this
project, or names other branches than the gates run for; when either
gate is missing, lacks the rule for a merge request or for a `batch*` branch,
has a `when: never`, a `when: manual`, a `changes:` list or `allow_failure`,
reads `SAST_DISABLED`, does not run its script with `auto`, or is not retried on
exit 2 and on `.gate-retry`'s reasons; when `semgrep-sast-gate` does not need
`semgrep-sast`'s artifacts; when one of the four scans on linix does not compare
a batch with `master`; when the release job's `collect` does not refuse a
committed report or an older one; when `build/land-via-mr.py` requires a job
the CI files do not have, or does not require the gates and `checkers`, its
`documents_only` disagrees with `.own-runners`' paths
([Landing on master](#landing-on-master)), its `GATE_FILES` leave out a file of
the gates' code, `.gitlab-ci.yml`, a CI file a gate job is in or reaches with
`extends` or `!reference` (followed from template to template: today
`mr-checks.yml`, `gitlab-security.yml` and `no-github.yml`), `sign-off.py` or
`scan-secrets.py`, which the landing runs, or `static-analysis.sh` or
`semgrep.sh`; its `GATE_CODE` is not `security-gate.py`'s; its `BASE_GATES` are
not the two gates; or its `BASE_COPY_SCRIPTS` do not name each gate with the
script its job runs; when a job that holds a change - the two gates,
`semgrep-sast`, `static-analysis`, `semgrep` and `checkers` - or a template one
extends or takes a part of is defined in a second CI file, under any name YAML
spells it with, since GitLab merges the second definition into the first, a
later include's keys over an earlier one's, and could take the job's rules,
script or `allow_failure` from a file the landing asks no person's word for;
when a CI file other than `.gitlab-ci.yml` sets `variables`, `default` or
`workflow` at its top level, which every job takes, or includes anything but
one of GitLab's templates; and when a Python script of `GATE_FILES` imports
anything but `os` and `sys` before it takes its own directory off its module
path (`PATH_GUARD`). Each of those has a case in its self-test that must fail.
`security-gate.py`'s own self-test fails while the tree's `static-analysis.sh`
or `semgrep.sh` has no `# scan-scope` marks around its tools' invocations, the
rule packs' fetching and what stands as the tools' report, or one of their
options outside them; while `semgrep-sast` pins another analyzer than the one
whose words its `SARIF_FIXTURE` holds; and while a copy of the gate, run beside
files named as the modules it imports, runs one of them.
`build/land-via-mr.py` lands a batch only when `checkers` succeeded itself,
not while allowed to fail, since every rule above runs in it.

**What they cannot hold.** The checks are against mistakes, not against a
hostile change: a change's own CI files say which jobs run and what they run,
and its own copies of the gate scripts run in them. A merge request from a fork
runs in the fork, with its own CI files, which can leave a gate out, on runners
its owner chooses, and GitLab's Free tier cannot make a job required: a fork's
change lands through a batch, or on a pipeline run here, and for a batch
`build/land-via-mr.py` checks that each gate ran in the project's own pipeline,
passed, and judged the batch against `master`'s tip, and asks for a person's
word that a change of the gates' own files was read; for any other merge
request the owner, who alone merges, reads every change to the CI files before
he merges it. The owner can also unprotect `master`, which
is break-glass ([Landing on master](#landing-on-master)). The gates leave out
what SECURITY.md says no gate reads - the scripts under `build/`, which
`semgrep-sast`'s analyzer leaves out, among them. Semgrep's registry rules, which `semgrep`
fetches on a batch, are not pinned (below). An exception is keyed to the text of
the flagged line, so an edit elsewhere in the same function can make an accepted
finding exploitable without changing its fingerprint; the exception for
`ParseIPv6_`'s copies says what would. A source file moved where `semgrep-sast`
does not read it under another name, or into a file already there, and
rewritten past what git pairs as a move, reads as a deletion and a test like any
other. The scripts of the two scans on linix -
`static-analysis.sh` and `semgrep.sh` - are held by a gate only in the lines
they mark as choosing what their tools read and report, and for the tools'
options outside them: a change elsewhere in them that narrows a scan another
way (a file deleted before a tool runs, an exit status taken for another) is
held by no gate, and a landing asks a person's word for any change of them
(`--gates-read`), as for a change to the gates' jobs; and those scans run the
change's own `security-findings.py`, whose filters decide what a finding is,
where the base's copy judges only the gates - a change of it needs that word
too. And a
project variable, which only the owner can set, reaches every job and stands
above a job's own: one named `SAST_EXCLUDED_PATHS` would replace the job's list.

**To be shown on fixture merge requests**, once these checks are on `master`:
merge requests from unprotected branches cut from `master`, each closed without
merging, each of which must fail its pipeline but the last. One adds NuGet's
`System.Text.Json` 8.0.0 to a lock file (`mr-dependency-check`); one adds a PyPI
version that a `MAL-` record names, in a new requirements file under
`lockfiles/` that no job installs (`mr-dependency-check`); one sets zlib's
version to 1.2.11 (`mr-dependency-check`, the native path); one adds a High
`strcpy` to the server's C++ (`semgrep-sast-gate`); one adds an exception
together with the finding it accepts (`semgrep-sast-gate`, which reads the
base's file); one adds a line to a baseline, and one a `nosemgrep`
(`semgrep-sast-gate`); one adds `--exclude` between `semgrep.sh`'s
`# scan-scope` marks together with a change to a C++ file
(`semgrep-sast-gate`); one adds a `.semgrepignore` naming a C++ file it changes
too, and one commits a `gl-sast-report.json` that says the scan found nothing
(`semgrep-sast-gate`); one adds a requirements line `name @ <url>`
(`mr-dependency-check`); and one edits the `README.md` at the root alone, which
passes with both gates run. One that adds an unlisted binary fails `checkers`,
and so does one that defines `CI_MERGE_REQUEST_DIFF_BASE_SHA` in a CI file.

Landing on master
-----------------

Every change reaches `master` through a merge request. A maintainer's work, and a
contributor's or Renovate's taken with it, is folded into a `batch*` branch, and
`build/land-via-mr.py` lands it:

```
python3 build/land-via-mr.py batch41 --dry-run
python3 build/land-via-mr.py batch41
```

Four batches have landed this way: `batch41` through !1 on 29 September 2026,
`batch42-docs` through !9 on 30 September, `batch42` (6.3.6) through !10 on 5
October and `batch43` through !12 on 6 October 2026, each on the owner's
override of the approval rule (step 6). The script needs git and `glab`, logged
in to gitlab.com as a Maintainer. In order, and stopping at the first refusal:

1. **Before anything is sent**, the batch must be on top of the remote's
   `master` (the project merges by fast-forward only), add no merge commit, and
   carry its author's `Signed-off-by` on every commit it adds, less those the
   project's `v*` tags already hold (`build/sign-off.py`, the check the
   `sign-off` job runs); and the secret scan of every commit it adds
   (`build/scan-secrets.py`, [Before a push](#before-a-push-the-secret-scan))
   must run and find nothing: a possible secret, or no scanner to run it,
   refuses (exit 8). The scan is the checkout's own `scan-secrets.py`, with
   the checkout's rules script, so the landing runs from a checkout of
   `master`, where none of the batch's code judges the batch ([What the
   secret scans read](#what-the-secret-scans-read)). A `batch*` branch's pipeline does not run
   `secret_detection`, and its merge request runs no pipeline, so this is the
   scan that reads a batch before it is on GitLab. A batch that changes the
   gates' own files - `GATE_FILES`: the five gate scripts; `.gitlab-ci.yml`,
   whose workflow rules, defaults and includes every job takes; the CI files the
   gate jobs are in or take a part from, `mr-checks.yml`, `gitlab-security.yml`
   and `no-github.yml`; the landing script and `check-release-chain.py`;
   `sign-off.py` and `scan-secrets.py`, which this step runs, and
   `secret-detection-rules.sh`, which makes both secret scans' rules; and the
   source scans' scripts, `static-analysis.sh` and `semgrep.sh` - needs
   `--gates-read "<who read it, and what>"`, a person's word that the change was
   read whole: the base's copy of the gate scripts judges such a change too, but
   only a person reads an edit of the jobs. Without it the script refuses (exit
   9); with it, the landing record keeps the reason. `--dry-run` stops here.
2. It pushes the branch. A `batch*` branch is protected, so its pipeline runs
   everything, the gates on the project's runners and the change checks
   included.
3. It opens a merge request into `master`, not squashed, with the source branch
   to be deleted, or finds the one already open for the branch: running it again
   resumes. `--no-wait` stops here.
4. It waits for the pipeline of exactly the branch's tip (twelve hours by
   default, `--timeout-hours`). A merge request from a `batch*` branch runs no
   pipeline of its own (the `workflow:` rules), so GitLab holds it to the
   branch's pipeline for the same commit, its head pipeline. Anything but
   success refuses: failed, canceled, skipped, or waiting on a manual job that
   blocks. Then it reads the pipeline's jobs, every page of them, since GitLab
   calls a pipeline a success with a job that failed while allowed to fail and
   with a job a rule never created. `semgrep-sast`, `semgrep-sast-gate`,
   `mr-dependency-check` and `checkers`, in which `build/check-release-chain.py`
   holds the CI files to the gates, must each have succeeded, and so must
   `static-analysis` and `semgrep` unless every file the batch changes from
   `master` is Markdown at
   the root or under a directory whose name begins with a dot - the changes for
   which `.own-runners` creates no scan. A gate that is not in the pipeline, was
   skipped, or failed, allowed to or not, refuses (exit 5), naming it; the
   statuses go into the landing record. The pipeline must be the project's own
   (its `project_id`), not one a fork ran, and each of `semgrep-sast-gate` and
   `mr-dependency-check` must say in its log - "The base: <commit>, ..." - that it
   judged the batch against `master`'s tip, where the batch leaves it: a base a
   CI file named in GitLab's stead would show there (exit 5). When the batch
   changes the gates' code (`GATE_CODE`, the five gate scripts), each must also
   say in its log that `master`'s copy of its own script judged the batch and
   passed it - "The base's copy of build/ci/security-gate.py exited 0." in
   `semgrep-sast-gate`'s, and of `build/ci/mr-dependency-check.py` in
   `mr-dependency-check`'s - or that `master` has no copy of it; a line the
   base's copy printed itself is not that, and without it the landing refuses
   (exit 5), since the gate's own code decides whether that copy runs.
5. GitLab must call the merge request mergeable. The project's settings make
   that mean the pipeline passed (*Pipelines must succeed*), every thread is
   resolved (*All threads must be resolved*), it is not a draft, and `master`
   has not moved since the batch was rebased (fast-forward merge).
6. It reads the approvals and decides whether somebody other than the author
   approved the tip it lands ([GOVERNANCE.md](../../GOVERNANCE.md), "Review").
   An approval given before that tip reached the merge request (the merge
   request's diff versions say when) does not count: whether a push takes
   earlier approvals off is a setting of the paid tiers. Without one it refuses
   (exit 7): from the first landing after 6.3.4 every merge request needs Zain
   Ul Abidin's approval, the owner's for Zain's own (the owner's decision of 26
   September 2026). The owner alone may override that for one landing with
   `--land-without-approval "<reason>"`, which the script refuses unless it runs
   as one of the owner's accounts, and whose reason it records in the landing
   note and its log.
7. It merges through the API at exactly the tip it checked (`sha`), with
   `squash` false - a squash would put one commit in the merge request author's
   name in place of the batch, its authors and its sign-offs - and checks that
   the remote's `master` is now that tip.
8. It records the landing in a note on the merge request - the tip, the
   pipeline, its gate jobs, the gates' base, whether the gates' own files
   changed and who read them, the sign-off, the approvals and who landed it - and
   in `land-via-mr.log` in the repository's git directory. It moves the local
   `master` to the tip when no worktree has it checked out, and deletes the
   branch on GitLab and locally where it can, saying when it cannot.

Nothing reaches `master` when it refuses. After a failed pipeline or an open
thread, change the batch, push it again, and run it again; after `master` has
moved, rebase the batch onto it first, which starts a new pipeline. A batch
lands on a pipeline that ran, never on a skipped one: a tip pushed with
`ci.skip` has a skipped pipeline, which step 4 refuses, and a pipeline started
for the branch through the API (`glab api -X POST projects/86730559/pipeline -f
ref=<branch>`) runs every job, the scans as the batch's changes from `master`
decide. After a refusal for a gate job (exit 5), find why the pipeline lacked
it - a gate a rule left out, that was skipped or that failed while allowed to
fail has not held the batch - and run the tip's pipeline again.

Merging by fast-forward is a push to `master`, so `master`'s own pipeline runs
the whole gate again on the same commit ([Which pipeline runs
what](#which-pipeline-runs-what)); the `sign-off` job does not run there, since
every commit a merge request brings to `master` has passed it on the way. A
direct push to `master`, which Maintainers can make until its push rule is *No
one* (below), is checked by nothing.

**What has run.** `python3 build/land-via-mr.py --self-test`, which `checkers`
runs, drives the whole landing against a bare repository standing for the
project and a GitLab API held in memory: a landing, and a refusal for each of an
unsigned commit, a possible secret, a secret scan that could not run or failed
(the scan itself stands in there; its own self-test covers it), a batch behind
`master`, a merge commit, a failed pipeline, a pipeline waiting on a manual job,
an open thread, and a missing approval once one is required; approvals read from
the merge request's system notes where the approvals API is refused; an approval
given before the tip reached the merge request not counted, and counted with a
note where the diff versions cannot be read; the release-tag exemption taken
from the project's tags, never from a tag only the local clone has; and the gate
jobs of step 4 - a pipeline of 115 jobs with the gates on its second page, a
pipeline with no `static-analysis`, `semgrep-sast-gate` failed while allowed to
fail, each change gate missing, a scan skipped, a documents-only batch landing
without the scans, a documents-only batch with no change gate, and Markdown
under a directory without a dot, which still needs the scans; a batch that
changes a gate script without `--gates-read` and with it, a pipeline another
project ran, a gate's log that names another base or none (a line of the
base's copy of a gate is not its own), and a batch that changes a gate script
whose gates' logs say nothing of the base's copy, only the copy's own line, or
that it failed - each refused - or that it passed, or that the base has none,
and one that changes `no-github.yml` alone, which needs `--gates-read` and no
word of the base's copy. On GitLab, the
four landings above ran the merge request, its head pipeline, the approvals and
diff-versions endpoints on the Free tier, the merge and the deletion of a
protected branch. The check of the gate jobs came with the change checks, and
its first run on GitLab is the landing of the batch that brought them.

### Nobody pushes to master, and a skipped pipeline does not count

Two project settings finish what the change checks and this script begin
([SECURITY.md](../../SECURITY.md), "Every change"). Both are the owner's, and
both wait until the batch that brings the change checks has landed through them:
until then a direct push is how a fix lands that cannot wait for a batch, as the
re-pinned analyzers and the exceptions of 6.3.6's release did on 5 October 2026
(`e3c448aa6`).

**`master`: push *No one*, merge *Maintainers*.** `master`'s rule lets
Maintainers push and merge. The owner makes the change in Settings, Repository,
Protected branches: for `master`, *Allowed to push and merge* becomes *No one*,
*Allowed to merge* stays *Maintainers*, and force push stays off. The edit is in
place. Through the API the same takes two calls on the Free tier, whose `PATCH`
of a protected branch cannot change an access level (`allowed_to_push` is
Premium's), so the branch is unprotected and protected again, and is unprotected
for the moment between the two:

```
glab api -X DELETE projects/86730559/protected_branches/master && \
glab api -X POST projects/86730559/protected_branches -f name=master \
  -F push_access_level=0 -F merge_access_level=40 -F unprotect_access_level=40 \
  -F allow_force_push=false
glab api projects/86730559/protected_branches/master
```

The last must show `push_access_levels` holding one entry, `access_level` 0 (*No
access*), `merge_access_levels` Maintainers, and `allow_force_push` false.
Nothing then pushes to `master`, the owner included: no hand's `git push`, and
no job (Renovate pushes `renovate/*` branches and opens merge requests; `pages`
and `forge-export` push nothing). A fix that cannot wait - an exception before a
release, a re-pinned analyzer - goes by a merge request from an unprotected
branch, whose pipeline is the checks and the change checks on GitLab's hosted
runners (about twenty minutes, estimated), and which the owner merges. That
pipeline runs on the project's compute minutes, which ran out on 9 October 2026:
while they are spent, it fails before it starts (`ci_quota_exceeded`), and such
a fix goes by a `batch*` branch, whose pipeline is the whole gate on the
project's runners ([Every job on the project's own
runners](#every-job-on-the-projects-own-runners)). Tags are not touched: the
release tag is still the owner's to push. The owner can still unprotect
`master`, or set the rule back to *Maintainers*: that is break-glass, and one
used is said, with its reason, in the next release's notes.

**A skipped pipeline does not count.** The project's
`allow_merge_on_skipped_pipeline` was unset on 6 October 2026, and GitLab
treated a skipped pipeline as one that had not passed (on 29 September 2026 a
tip pushed with `ci.skip` left !1 at `detailed_merge_status` `ci_must_pass`).
Set to false, that is the project's own setting rather than a default:

```
glab api -X PUT projects/86730559 -F allow_merge_on_skipped_pipeline=false
glab api projects/86730559
```

The last must show `allow_merge_on_skipped_pipeline` false. The landing script
refuses a skipped pipeline whatever the setting (step 4).

The runners
-----------

Three machines take the project's own jobs: linix and server2, whose runners
carry the same three tags and share the jobs of the tag `linix` between them
(linix has a second entry of its own, `linix-systemd`), and bench150, the
Windows bench.

### linix

The Ubuntu 26.04 lab VM on the maintainer's laptop, at 192.168.11.154: 16 cores,
32 GB of memory, 200 GB of disk (2 October 2026). GitLab Runner 19.4 runs there
as a system service, with the docker executor and six CPUs and 8 GB of memory
for each job. The default
image is `ubuntu:26.04`, the pull policy is `if-not-present`, and the runner is
not privileged. It has the tags `linux`, `docker` and `linix`, and it is runner
56743206 on GitLab.

**It runs up to three jobs at once.** `/etc/gitlab-runner/config.toml` says
`concurrent = 3` for the service and `request_concurrency = 3` for this entry:
one job at a time until 28 September 2026, then two, and three the same night.
Three jobs of six CPUs each ask for more than the sixteen cores, so jobs side by
side slow each other down. The jobs that must not run beside their own kind say
so with a `resource_group` - `linix-suite` for the two whole suites
(`linux-suite` and `alpine-suite`), `linix-package-build` for the package builds
(the distribution releases' `.deb` and `.rpm` files, the Alpine package, the
source packages and the Nix build) - and wait for one another across pipelines;
the rest take whichever of the three places is free.

What the host needs:

- Docker (29.1 today) and the runner.
- The job image, built on the host and pushed from there to the project's
  registry as `registry.gitlab.com/hmailserver/hmailserver/ci-linux:<tag>`, which
  the first job to need it pulls by its digest (see [The image](#the-image)). The
  project is public, so the pull needs no credential. If `config.toml` restricts
  images, that name must be allowed.
- `postgres:18` at the digest the CI file names. It is pulled on first use, and
  it is on the host already.
- The other images the ported jobs name, each by digest, each pulled on first
  use: `mariadb:13` (the MySQL upgrade gate's service) and `ubuntu:26.04` from
  Docker Hub, `registry.fedoraproject.org/fedora:44`, and
  `gcr.io/kaniko-project/executor:v1.24.0-debug`. If the runner's `config.toml`
  restricts images (`allowed_images`) or registries, these must be allowed.
- The images of the jobs that take `default:`'s tags (since 9 October 2026), each
  by digest, each pulled on first use: Docker Hub's `python:3.14-slim-trixie`,
  `node:24-trixie-slim`, `alpine:3.24`, `ubuntu:26.04`, `debian:13`,
  `archlinux:base`, `regclient/regctl:v0.11.6-alpine` and `aquasec/trivy:0.74.0`
  (`container_scanning`'s), Microsoft's `mcr.microsoft.com/dotnet/sdk:10.0`,
  `gcr.io/projectsigstore/cosign`, and the project's copies of GitLab's analyzers
  (`mirror/semgrep`, `mirror/secrets`).
- The runner's local cache, which carries the fuzz corpus from night to night
  (`cache:` in `linux-fuzz`). The docker executor keeps it in volumes on the
  host, one for each of the three places a job can run in, so a night's job
  starts from the corpus of the place it was given and leaves its corpus there;
  nothing needs setting up.

Nothing else is installed on the host for CI. The toolchains, the .NET SDK,
PowerShell, Syft and the database clients are in the image.

The same runner service has a second entry, `linix-systemd`, runner 56932743 on
GitLab (1 October 2026). It is privileged - its jobs' services are not
(`services_privileged = false`, since 2 October 2026) - has the one tag
`linix-systemd`, and, like the first, takes protected refs only and no untagged
job. Its other settings are the first entry's: the docker executor,
`ubuntu:26.04`, six CPUs and 8 GB for each job, `if-not-present`. It asks for one
job at a time (no `request_concurrency`), and its jobs count towards the same
three as the first entry's: the service's `concurrent` is for both.
`package-systemd-ubuntu26.04` and `package-systemd-fedora44` use it, since podman
needs privilege to run the container systemd runs in inside the job's. That inner
container is not privileged, and its image masks `systemd-binfmt`
([PackagesDeb.md](PackagesDeb.md), "Started under systemd", says why). A job on this
entry is root on linix in effect, so nothing that runs a merge request's or a
fork's code may take its tag.

### server2

A machine of the owner's at 10.0.0.19, which has taken the project's jobs since
5 October 2026 and runs services of the owner's own beside them: Ubuntu 26.04,
16 cores (32 threads) and 182 GB of memory (6 October 2026). GitLab Runner 19.4
runs there in a container of its own, with the docker executor, 28 CPUs and
40 GB of memory for each job, and up to three jobs at once. It is runner
57072507 on GitLab, registered on 5 October 2026 at 18:37 UTC as linix's first
entry is - a project runner, `ref_protected`,
`run_untagged` off, `locked`, a four-hour `maximum_timeout` - and with the same
tags, `linux`, `docker` and `linix`.

**It shares the tag `linix`.** No job names server2 alone: every job tagged
`linix` - each job of `.linix`, every job that takes `default:`'s tags, and the
others the tables above give to linix -
goes to whichever of the two runners asks GitLab for it first with a place
free, so *linix* in those tables names the tag, not the machine, and up to six
such jobs run at once, three on each. A `resource_group` belongs to GitLab, not
to a runner, so `linix-suite` and `linix-package-build` still run one job of
their kind at a time across both machines. `linix-systemd` is an entry of
linix's alone, so `package-systemd-ubuntu26.04` and `package-systemd-fedora44`
run only there. server2 needs what linix needs (above): the job image, which it
pulls from the project's registry by the digest `.linix` names, as linix does,
the images the ported jobs name, and qemu-user's binfmt handler for the arm64
jobs, which they check for first (`build/ci/arm64-emulation.sh`). server2 has no
qemu-user package and no `binfmt.d` entry. The handler was registered there by
hand on 5 October 2026, and since 6 October 2026 a unit of the host's,
`hmlab-binfmt.service` (enabled, a oneshot ordered after Docker), registers it
at every boot: it runs
`tonistiigi/binfmt@sha256:400a4873b838d1b89194d982c45e5fb3cda4593fbfd7e08a02e76b03b21166f0`
with `--install arm64`, which registers QEMU 10.2.3's `qemu-aarch64` with the
flags `POCF`, so the arm64 jobs that server2 takes find it after a reboot as
linix's do. What a job keeps in the runner's cache stays on the machine that ran
it: a night of `linux-fuzz` or a week of `forge-export` that server2 takes finds
neither the corpus nor the kept archives that linix's cache volumes hold, and
leaves its own on server2.

**What 6.3.6's pipeline showed.** server2 came online while 6.3.6's tag
pipeline 2914690858 was running, and took 40 of the 106 jobs that ran in it,
linix 38: among them `source-tarball`, `linux-cross`, `linux-sanitize`,
`static-analysis`, `semgrep`, both AppImages and their tests, package builds and
install jobs of both architectures, and `container-image-amd64`,
`container-image-arm64`, `container-image-sbom` and `container-image-start-arm64`
(linix ran `container-image-start`). Four of its jobs failed there and passed
when run again, each on linix: `linux-suite` was stopped at its script timeout,
3 h 50 min, in a suite whose wait for each REST restart `beb9c0587` has since
shortened (that wait had cost a run on server2 about 1 h 40 min);
`rpm-fedora43-aarch64` and
`rpm-leap16-aarch64` failed `imip-tests`' timing check under qemu-user, whose
margins `c4edb3eaf` has since put under `HM_TEST_TIME_BOUND`; and
`release-packages` could not resolve `gitlab.com` through the machine's
resolver to download its needs' artifacts.

### bench150

The second Windows regression bench at 192.168.11.150: Windows Server 2025 with
16 cores and 16 GB of memory. GitLab Runner runs there as the `gitlab-runner`
service, **as LocalSystem**, with the shell executor and `shell = "pwsh"`.
Its builds are in `C:\GitLab-Runner\builds`. It has the tags `windows`, `bench`
and `bench150`, and it is runner 56743216 on GitLab.

What the host needs is everything the Windows bench needs
([RegressionEnvironment.md](RegressionEnvironment.md), the list for a second
bench), plus these:

- **PowerShell 7 installed for the machine**, in `C:\Program Files\PowerShell\7`
  and on the runner's PATH. LocalSystem sees the machine PATH only, and a
  PowerShell installed from the Microsoft Store is on no PATH but its
  installing user's. A service reads the machine PATH when it starts, so the
  runner service has to be restarted after PowerShell 7 is installed, and a
  PATH change can need a reboot before a service sees it. On bench150 the
  runner's PATH is not the machine PATH at all: the `gitlab-runner` service has
  an `Environment` value of its own (`HKLM\SYSTEM\CurrentControlSet\Services\gitlab-runner`),
  whose `PATH` is `C:\Program Files\PowerShell\7` followed by the machine
  PATH as it was then (read on 29 September 2026; the machine PATH itself has
  no PowerShell 7 in it). A tool a job needs must be on that `PATH`, which a
  change to the machine PATH does not reach, or found by the job itself, as
  Strawberry Perl and libpq's build tools are for `windows-packages-arm64`
  ([PackagesWindows.md](PackagesWindows.md), "The ARM64 libraries").
- Git on the machine PATH (`C:\Program Files\Git\cmd`), which it is.
- `git config --system --add safe.directory C:/bench/hmailserver`. That checkout
  belongs to the Administrator, and the runner is LocalSystem. No other
  directory needs it: the job's own directory holds no repository
  (`GIT_STRATEGY: none`).
- Access to `gitlab.com`, to fetch the commit, and to `api.nuget.org`, because
  the first build as LocalSystem restores NuGet packages into LocalSystem's own
  profile.
- For `windows-dotnet`: the .NET SDK for the machine, 10.0.303 or a later
  feature band of .NET 10 (`global.json`), on the machine PATH - 10.0.401 is
  there, in `C:\Program Files\dotnet` (read on 25 September 2026); and Visual
  Studio's Build Tools with the C++ tools and the .NET Framework SDK, which give
  `build/generate-com-wrapper.ps1` MIDL (through `vcvars64.bat`) and `TlbImp.exe`
  ("NETFX 4.8.1 Tools"). The gate's server build needs the Build Tools already.
  This job clones the commit into the runner's own builds directory as usual;
  its checkout is LocalSystem's, so it needs no `safe.directory`.
- For `windows-installer`: Inno Setup 6 for the machine, in
  `C:\Program Files (x86)\Inno Setup 6`, because LocalSystem sees no per-user
  installation; and access to `gitlab.com`'s package registry, `7-zip.org` and
  Microsoft's .NET download site, which `get-installer-binaries.ps1` and
  `get-dotnet-runtime.ps1` fetch from, each file checked against its pinned
  hash. `HM_ISCC`, a CI/CD variable, names `ISCC.exe` where it is somewhere
  else. Installed on 26 September 2026: Inno Setup 6.7.3, the maintainer's own
  copy that builds the release installers, copied into that folder without its
  per-user uninstaller (nothing registered; every executable and DLL
  Authenticode-signed by Pyrsys B.V.). It was not installed through winget,
  because the winget manifest's installer, like jrsoftware.org's download
  page, is on `github.com`. Deleting the folder removes it.
- For `release-authenticode`: Microsoft's Artifact Signing Client Tools, which
  bring SignTool's Artifact Signing plugin (`Azure.CodeSigning.Dlib.dll`); the
  plugin asks for .NET 8 with `rollForward: Major`, so the machine's .NET 10
  runtime runs it. SignTool itself is the Windows SDK's, which the Build Tools
  installed. `HM_SIGNTOOL` and `HM_SIGNING_DLIB` name either where it is
  somewhere else. Installed on 26 September 2026, version 0.1.128, in
  `C:\Program Files\Microsoft\ArtifactSigningClientTools`. `winget install
  --exact --id Microsoft.Azure.ArtifactSigningClientTools --scope machine`
  finds no installer (the manifest declares no scope), and without `--scope`
  the MSI installs into the administrator's own profile, which LocalSystem
  does not see; so the MSI winget downloads (from `download.microsoft.com`) is
  installed with `msiexec /i ArtifactSigningClientTools.msi
  TARGETDIR="C:\Program Files\Microsoft\ArtifactSigningClientTools\"
  ALLUSERS=1 /qn`. `msiexec /x {2EF3A45E-5812-4982-A0EB-1A3609722F99} /qn`
  removes it; never run the `Uninstallation.bat` or `UninstallDotNetFolder.ps1`
  it ships, which delete all of `C:\Program Files\dotnet`.
- For `windows-packages-arm64`: Visual Studio's ARM64 tools, ATL for ARM64 and
  the ARM64 C++ runtime in the Build Tools instance, and the Windows SDK's ARM64
  libraries, which `build/ci/packages/check-arm64-toolchain.ps1` checks
  (all there on 29 September 2026); Strawberry Perl in `C:\Strawberry`, which
  `libraries\build-common.ps1` finds without the PATH (5.42.2 there); and
  access to `deb.debian.org`, `archives.boost.io`, `ftp.postgresql.org`,
  `api.nuget.org`, `files.pythonhosted.org` and `downloads.sourceforge.net` (and
  the mirror it sends a download to) for
  the ARM64 libraries' sources and libpq's build tools, each held to its pinned
  SHA-256 ([PackagesWindows.md](PackagesWindows.md)).
- **Nothing from GitHub.** Since 29 September 2026 bench150's hosts file sends
  `github.com`, `api.github.com`, `codeload.github.com`,
  `objects.githubusercontent.com`, `raw.githubusercontent.com` and `ghcr.io` to
  `0.0.0.0` and to `::`, between two comment lines that name this section, so
  that anything a job runs that reaches for GitHub fails by name ("The remote
  name could not be resolved" from .NET) rather than quietly working. The file
  as it was before is `C:\bench\hosts.before-github-block`; removing the lines
  between the two comments undoes it. After the change `windows-packages-arm64`
  and `windows-packages-x64` both built their installers by hand as LocalSystem
  under the gate lock ([PackagesWindows.md](PackagesWindows.md), "What was run
  by hand on bench150").
- **Nothing a cancelled job left running, before a checkout.** In pipeline
  2897966924 (30 September 2026) a cancelled build left `cl.exe` and
  `mspdbsrv.exe` running in the runner's checkout, holding a precompiled header
  open, and the next job's checkout of that directory failed until they were
  killed by hand: GitLab stops a cancelled job's process tree, but `mspdbsrv`
  detaches by design and a compiler started through MSBuild's tracker can
  outlive its parent. Since 6.3.6 every job on bench150 takes
  `.bench150-leftovers`' `pre_get_sources_script` (`.gitlab-ci.yml`), which runs
  before anything is fetched and stops each process of the runner's own account
  (LocalSystem) named `cl`, `link`, `lib`, `mspdbsrv`, `midl`, `rc`, `cvtres`,
  `mt`, `vctip`, `MSBuild`, `Tracker`, `VBCSCompiler`, `csc`, `dotnet`,
  `testhost` or `ISCC` whose working directory is the job's `CI_PROJECT_DIR`
  or under it, or whose command line names it, and says in the log which and
  why. It never stops another account's process (the maintainer's split gate),
  its own shell or the runner, a process in another job's directory, or
  `hMailServer.exe`, which is not on the list; the gate's service runs from
  `C:\bench\hmailserver`, outside the runner's builds. The working directory is
  read from the process's own memory (its PEB, 64- or 32-bit), which Windows
  shows nowhere else. A check that cannot run says so and the job goes on.

### Re-creating a runner

The integrator's two scripts, `make-linix-runner.py` and
`make-bench150-runner.py`, create a runner. They are kept with the integrator's
tools, not in the repository. Each one does this:

1. It creates the runner through GitLab's API (`POST /user/runners`). The runner
   is a project runner of project 86730559 with its tags, `ref_protected`,
   `run_untagged` false, `locked`, and a four-hour `maximum_timeout`.
2. It writes the runner's `config.toml` on the machine, with the token going
   from GitLab's answer into the file over the SSH session's input. The token
   is never printed.
3. It starts the service, and on bench150 it sets `safe.directory`.

The settings above are the ones that matter. With them, a runner can be made by
hand from GitLab's *New project runner* page and `gitlab-runner register`. Delete
the old runner on GitLab first, so that its token stops working.

The image
---------

The Linux jobs that build or run the server - `linux-build`, `linux-build-gcc`,
`linux-suite`, `linux-cross`, `linux-sanitize`, the two upgrade gates,
`linux-sbom`, `linux-repro` and `linux-fuzz` - run in one image, described by
`build/ci/linux.Dockerfile`. The package jobs run in the distributions' own
images instead, because the point is a clean system, and `linux-image` in
kaniko's. The image holds:

- Ubuntu 26.04 at a pinned digest;
- the toolchain and the `-dev` packages of the Linux build: clang, which builds
  what the project ships, and the distribution's default GCC, g++ 15, which
  `build-essential` depends on and which `linux-build-gcc` builds with; clang
  compiles against that GCC's libstdc++;
- clang's sanitizer and libFuzzer runtimes (`libclang-rt-dev`) and
  `llvm-symbolizer` (`llvm`), for `linux-sanitize` and `linux-fuzz`;
- the AArch64 cross GCC (`g++-aarch64-linux-gnu`), for `linux-cross`;
- `rpm`, whose `rpmbuild` CPack's RPM generator runs;
- GnuPG, the PostgreSQL client, the MariaDB client and `libmariadb3` (the
  library the server loads to reach MySQL), curl and Python;
- 7-Zip (`7zip`), whose `7z` the server runs to write and read a backup, for
  the suite's backup tests;
- the .NET 10 SDK and PowerShell 7, copied from Microsoft's SDK image at a
  pinned digest. Ubuntu's own `dotnet-sdk-10.0` is a 10.0.1xx SDK, and
  `global.json` refuses it;
- Syft, copied from Anchore's image on Docker Hub at a pinned digest.

Every package is Ubuntu's own. Nothing in the image, and nothing any job
fetches, comes from GitHub; [Where the tools come from](#where-the-tools-come-from)
lists every tool of every job.

The image is built **on linix**, by hand, and pushed from there to the project's
container registry, `registry.gitlab.com/hmailserver/hmailserver/ci-linux`, which
anyone can pull from, since the project is public. `.gitlab-ci.yml` could name it
in one of two ways, and `build/check-ci-image.py` accepts either, never both at
once; since 6 October 2026 it names it by digest:

- **By digest, from the project's registry.** `build/ci/build-linux-image.sh
  --push` pushes it as
  `registry.gitlab.com/hmailserver/hmailserver/ci-linux:26.04-<hash>` and prints
  that name with the digest of its manifest,
  `ci-linux:26.04-<hash>@sha256:<digest>`, which `.linix` then names, with the
  `if-not-present` pull policy: the first job on each runner with the tag
  `linix` pulls it, and Docker takes an image named so only as the registry
  holds that digest. `.linix`'s `before_script` is then `python3
  build/check-ci-image.py --job`, and so is that of `container-image-sbom` and
  `container-image-start`, which take `.linix`'s image without extending
  `.linix` and so would not have its `before_script`;
  `container-image-start-arm64`, which extends `container-image-start` with a
  `before_script` of its own, begins that with `.linix`'s. That step stops every
  job in the image before it builds anything - the release's x86-64 `.deb` and
  `.rpm`, made in `linux-build`, made again in `linux-repro` and checked in
  `release-verify`, and the container image's SBOMs, among them - unless the
  image it was started in (`CI_JOB_IMAGE`) is named so, with the tag of the
  commit's own Dockerfile. The image was first pushed so on 6 October 2026, at
  about 03:55 UTC, from linix:
  `ci-linux:26.04-52e9976e73bf@sha256:4eead00163c2c0b30108de94d861ee0c8b260578adb44d495fa47b535617eae9`,
  which `.linix` named until 8 October 2026, when the image was built again
  from the same Dockerfile, for Ubuntu's kernel-header and libc updates
  (`linux-libc-dev` 7.0.0-38.38, `libc6` 2.43-2ubuntu2.4, `openssl`
  3.5.5-1ubuntu3.7), and pushed: the same tag, moved to
  `sha256:350484026f1ade2f1c8bb0b9248fcbb32bf45394c40e9a89af4f92445e31d2d6`,
  which `.linix` has named since, and both runners with the tag, linix and
  server2, pull it by that digest. The 6 October digest was left with no tag,
  which the registry does not keep: GitLab.com's registry removes an untagged
  manifest that nothing references about 24 hours later (its online garbage
  collection; the cleanup policy, switched off, governs tags alone). So on 8
  October, within that day, the maintainers gave it a dated tag by hand,
  `26.04-52e9976e73bf-20261006`, by a copy inside the registry that preserves
  the digest (`skopeo copy --preserve-digests`, from the digest to the tag,
  with a login of the moment): the registry's tag list shows both tags since,
  the dated one answers that digest, and the commits that name it - `master`
  at 9b2001ce9, `batch44` - pull it as before. Since then `--push` keeps the
  previous digest itself, before the tag moves; step 2 below says the rule.
- **By its tag on the runner**, `hmailserver-ci/linux:26.04-<hash>`, found in
  the runner's own image store and never pulled: the only form until the second
  security review of 6.3.6 (SUP2-1), and the one `.gitlab-ci.yml` kept until the
  image was pushed on 6 October 2026, each runner with the tag `linix` then
  holding a copy of its own. A tag is only a name. Anything that drives the
  runner's Docker can give it other bytes, and every later job, a release's among
  them, runs in them.

What the digest holds against is a name given to other bytes - `docker tag`, or
an image built or loaded under the same name. It is no proof against root on
linix or server2 rewriting Docker's store underneath it, or the runner: a job on
linix's privileged `linix-systemd` entry (above) is root on linix in effect and
could change an image's files in place, which no check made inside a job can
see. That reach is the maintainers' already, since only they start a protected
pipeline, and it is why that entry's tag is kept from merge requests and forks.

The tag is made from the Dockerfile: `26.04-<hash>`, where `<hash>` is the first
twelve hex digits of the Dockerfile's SHA-256 with its line endings made LF.
`build/check-ci-image.py`, one of the checkers, fails until the CI files -
`.gitlab-ci.yml` and every file under `.gitlab/ci/` - name the image with the
tag of the Dockerfile as it is, and fails for a registry name without a whole
`sha256` digest, or with two digests, or for both kinds of name at once. Once the
image is named by digest it also fails while `.linix`'s `before_script` is not
the `--job` step, or while a job runs in the image without starting with it: one
that takes `.linix`'s image (`image: !reference [.linix, image]`) without its
`before_script` (`!reference [.linix, before_script]`), or one that extends
`.linix` or such a job with a `before_script` of its own that leaves the step
out - and while a job that extends `.linix` names an image of its own and keeps
the step, which would refuse it. So an edit to the Dockerfile goes like this:

1. Edit `build/ci/linux.Dockerfile`.
2. On linix, from a checkout of the commit, as a member of the `docker` group and
   signed in to the registry in a Docker configuration of the moment
   (`export DOCKER_CONFIG="$(mktemp -d)"`, then `docker login registry.gitlab.com`
   with a token of yours that may write the project's registry,
   `write_registry`), so that the token is not left in linix's own, build and
   push the image:

       bash build/ci/build-linux-image.sh --push

   Then `docker logout registry.gitlab.com` and remove that directory. The 6
   October 2026 image was pushed so. The script hands that configuration's
   file to skopeo's container for the copy below, read-only and for the
   destination alone, and refuses, before it builds, one that keeps its login
   in a credential helper, which the container cannot reach, as
   `build/ci/mirror-semgrep.sh` does. The script prints the line to write,
   `name: registry.gitlab.com/hmailserver/hmailserver/ci-linux:26.04-<hash>@sha256:<digest>`,
   and the checker holds the commit to it. It removes no older tag, because a
   pipeline for an older commit still asks for its own image. It lists them
   instead, and `docker image rm <tag>` removes one from linix that is no longer
   wanted; the registry keeps every tag of `ci-linux` too, and its cleanup
   policy, were it switched on, would keep them by name ([The container
   image](#the-container-image), "What the owner sets on the registry").
   A Dockerfile that has not changed builds again to the same tag, and
   `--push` then gives that tag in the registry to the new image. The digest
   an older commit names would be left with no tag, and GitLab.com's registry
   removes an untagged manifest that nothing references about 24 hours later
   (its online garbage collection, which no cleanup policy governs: a cleanup
   policy keeps or removes tags). So before the push the script reads the
   digest the registry's tag names now - without credentials, as a runner
   pulls it: `skopeo inspect --raw` in a container, the bytes hashed - and,
   unless the image about to be pushed carries that digest already, gives it
   the dated tag `26.04-<hash>-<yyyymmdd>` (the UTC date) by a copy inside the
   registry that preserves the digest (`skopeo copy --all --preserve-digests`,
   the login's file mounted read-only for the destination alone, as
   `build/ci/mirror-semgrep.sh` does), reads the dated tag back and holds it
   to the digest, and only then pushes; it prints the pin line and the dated
   tag with the digest it keeps. A second rebuild the same day finds the dated
   tag taken: when it names the digest to keep, nothing more is done, and
   otherwise the first free of `-<yyyymmdd>-2`, `-3`... is used. A tag new to
   the registry, a changed Dockerfile's, has nothing to keep. When the
   registry cannot be read, or the read-back is not the digest, the script
   stops before the push rather than move the tag off a digest it could not
   keep. That is the rebuild rule, for Ubuntu's updates between edits of the
   Dockerfile: a rebuild at the same Dockerfile moves the tag to a new digest,
   the pin in `.linix` follows it in a commit of its own, and the commits
   before that one keep their image under the dated tag (first on 8 October
   2026, for the kernel-header and libc updates above, when the 6 October
   digest was kept by hand, as `26.04-52e9976e73bf-20261006`, within the hour
   after the tag moved off it). `build/check-ci-image.py` holds the script to
   those steps, in their order, and its self-test runs the script against a
   docker of its own and a registry of files.
   (Without `--push` it builds `hmailserver-ci/linux:<tag>` alone, the earlier
   form.)
3. Put that line into `.linix` in `.gitlab-ci.yml`, in the same commit as the
   Dockerfile.
4. If the image was not pushed, the Linux jobs fail at their first step, naming
   the image they could not pull. That is by design. The alternative is a job
   that runs on an image made from an older Dockerfile, or on other bytes under
   the right tag, and says nothing.

**Why the image is built by hand and not by a job.** A job that builds an image
needs the host's Docker socket mounted into the runner's containers. That would
give every job on linix root on linix: every job for a protected ref, and every
job any future change to the rules lets through. The integrator decides on
socket access, and the image is built by hand. The second runner entry,
`linix-systemd` (above), is privileged, so a job that takes its tag is root on
linix in effect and can reach linix's image store: since it was added, the tag
rule alone no longer makes sure that no job runs on an image the Dockerfile does
not describe - a tag pins a name, not content - and the digest above holds the
jobs to the image the registry has, within the limit just said. (`linux-image`
builds the product's image without a socket, with kaniko,
but kaniko writes a tarball or pushes to a registry: it cannot put an image
into linix's own image store.)

The GCC build
-------------

`linux-build-gcc` (`.gitlab/ci/gcc.yml`) is `linux-build` with `HM_COMPILER=gcc`:
the same script, `build/ci/linux-build.sh`, builds the server and every
`*-tests` program with the distribution's default GCC instead of clang, runs the
testers and the OpenPGP writer's round trip through GnuPG, and makes no
packages - the x86-64 `.deb` and `.rpm` a release carries are `linux-build`'s,
built with clang. A distribution builds its own package of the server with its
own default compiler, which is GCC on Debian, Ubuntu, Fedora, openSUSE and
Alpine. The `deb-<distro>-amd64` jobs (`.gitlab/ci/packages-deb.yml`) already
build natively with Debian 12's, Debian 13's and Ubuntu 24.04's GCC (12, 14 and
13) and run the testers, and `deb-ubuntu26.04-arm64` cross-compiles the
release's AArch64 `.deb` with GCC; none of them builds natively with the newest,
Ubuntu 26.04's GCC 15, or runs the OpenPGP round trip. `linux-build-gcc` does,
so a change that only clang, or only an older GCC, accepts is found before a
distribution's maintainer finds it.

It runs for a push to `master` or a `batch*` branch that changes more than
documentation (`.own-runners`' rules), never for a `v*` tag - the batch's pipeline
built the same commit, and `release` does not need it - and never for a merge
request or a schedule. The compiler was already in the image: `build-essential`,
which the Dockerfile installs, depends on `g++`, Ubuntu 26.04's GCC 15
(`apt-cache depends build-essential`), so the job needs no image of its own and
the image's tag did not change. The GCC census by hand,
`bash build/linux-tu-census.sh --cc g++`, still compiles every core file alone.

Measured in WSL on 28 September 2026, Ubuntu 26.04 with GCC 15.2.0, `ninja -j4`
on a laptop shared with another build, over the tree of 6.3.6's batch at
`4bdbf09ca`: the 799 build steps took 25.5 minutes with no error, and all four
tester programs passed (`object-store-signature-tests`, `script-binding-tests`,
`snmp-tests`, `spf-tests`), as did `--version` and `--help`. GCC printed 5,550
warning lines, 5,537 of them `-Wdeprecated-declarations` from one header,
`Common/Util/StdString.h` (`std::binary_function`, `std::unary_function` and
`std::bind2nd`, repeated in every file that includes it); the thirteen others - `-Wenum-compare` in
`VariantDateTime.cpp` (two enums with the same values), `-Wformat-truncation` in
`DomainReports.cpp`, `ObjectStoreSignature.cpp` and `SamlSignIn.cpp`,
`-Wnonnull` in `CrashSimulation.cpp` (whose null write is the point) and the
rest - are for the job's log to show, not to fail it: the job fails on an error
or a tester, as `linux-build` does.

The Windows gate
----------------

`windows-gate` works in `C:\bench\hmailserver`, not in the runner's builds
directory, because the bench's hMailServer service is registered from that
path. The bench keeps its registration. The job has `GIT_STRATEGY: none`, so the
runner clones nothing, and the job does the following itself:

1. It fetches the commit into the bench's checkout from
   `https://gitlab.com/hmailserver/hmailserver.git` (`HM_GATE_REPOSITORY`).
   The project is public, so no credential is needed. Git's credential helper
   and every prompt are switched off, because under LocalSystem nobody can
   answer one. A fetch writes under `.git` only, so it is safe before the lock
   is held.
2. It takes the commit's own `build/ci` out of the object store with
   `git archive`, into the job's directory. The gate scripts that run are the
   commit's own, and nothing in the bench's working tree has changed yet.
3. It takes the gate lock (below).
4. It runs `build/ci/windows-gate.ps1`, which does the following. These are the
   steps of the remote half's `remote-gate.ps1`, versioned in the tree:
   1. It checks the commit out, and stops with exit 5 when HEAD is not that
      commit.
   2. It upgrades the bench database through the running service
      (`upgrade-test-database.ps1`, exit 8).
   3. It stops the service and builds the Release server (`build.ps1`, exit 3),
      then runs `post-build.ps1`.
   4. It removes the old test assembly and builds the tests (`build-tests.ps1`,
      exit 6). A test assembly more than 30 minutes old is refused as stale
      (exit 7).
   5. It starts clamd if nothing listens on 3310, and waits a minute at most
      for it, then runs `preflight-tests.ps1 -Clean` (exit 2). Both read the
      listening ports with `build/ci/tcp-listeners.ps1`: from
      `Get-NetTCPConnection`, or, when that returns no TCP endpoint at all, from
      .NET's `IPGlobalProperties` or `netstat -ano`, and say which. On server1,
      from 9 October 2026, the CIM provider behind `Get-NetTCPConnection`
      answered nothing while every listener was there.
   6. It restarts the service and watches the idle server for two minutes of
      error logs (exit 9).
   7. It runs `run-tests.ps1`: the whole suite in one run, or the subset that
      `HM_GATE_WHERE` names. Exit 0 means nothing failed. Exit 1 means tests
      failed. Exit 4 means the suite did not run to a result, or ran no test.
5. It releases the lock. `after_script` releases it again, for a job that was
   cancelled (GitLab runs `after_script` then, from 17.0) or ran out of time
   (`RUNNER_SCRIPT_TIMEOUT` stops the script ten minutes before the job's
   timeout, so that `after_script` still runs). It also starts the service again
   if such a job left it stopped.

The job itself adds four exit codes: 10 when the lock stayed another holder's
for `HM_GATE_LOCK_WAIT_MINUTES` (90), 11 when the commit could not be fetched,
12 when the commit has no `build/ci`, and 13 when the lock could not be taken
for any other reason, or stayed another holder's in a `v*` tag's pipeline.

**A busy bench is a warning, not a failure.** Exit 10 is
`allow_failure: exit_codes: [10]`: the pipeline shows the job with a warning
and stays green, since nothing was wrong with the commit, only with the
timing. Retrying the job gates it. `windows-dotnet` does the same after its 200
minutes. A `v*` tag's pipeline turns the same wait into exit 13, a failure,
because `release` waits for `windows-gate` and `windows-dotnet` and must not be
startable when the suite never ran; `build/check-release-chain.py` holds both
jobs to that.

**The whole suite, in one run.** `HM_GATE_WHERE` is empty, and an empty value
runs every test (3,490 test cases on 25 September 2026). The two halves of the
split gate together cover every namespace of the suite (RegressionEnvironment.md
names them), so one run with no filter is the same set of tests. When a second
Windows runner exists, two copies of the job, each with half the filter in
`HM_GATE_WHERE`, split the suite again.

**What changes under LocalSystem.** The split gate's remote half ran as the
Administrator over SSH. The job runs as LocalSystem, the same account as the
service:

- The DPAPI failure after a reboot does not happen. `ClientCertificateFactory`
  imports each TLS fixture's certificate with a user key set, and under a
  key-only SSH logon the Administrator's DPAPI master key stayed locked after a
  reboot. Fourteen SSL tests then failed with "Access denied" until somebody
  logged on with the password. LocalSystem's key is always open.
- The tests' temporary files go to `C:\Windows\Temp` instead of the
  Administrator's profile. The service is LocalSystem too, so it reads them as
  before.
- NuGet restores into LocalSystem's profile. The first build downloads every
  package again. `vswhere` finds `MSBuild\Current\Bin\MSBuild.exe`, which is
  32-bit, and a 32-bit process's view of
  `C:\Windows\System32\config\systemprofile` is redirected to `SysWOW64`. Only a
  `PackageReference` project would notice, and the gate builds none: the test
  project uses `packages.config`, restored beside the solution.
- `RestApiAccountImport` finds no `python.exe` (the Store's stand-in is the
  Administrator's, and never on LocalSystem's PATH). It skips, as it did before.
- Git's credential helper (`manager`) is switched off for the fetch, and
  `MSBUILDDISABLENODEREUSE=1` stops MSBuild from leaving worker processes
  running as LocalSystem after the job.
- Every PowerShell the gate starts is started by the path of the one running
  it, not by name.

The gate lock
-------------

Two things run gates on bench150: this job, and the maintainer's split gate
(its remote half, over SSH). Either one checking a commit out under the other's
running suite makes a gate that means nothing. Both take one lock,
`C:\bench\gate.lock`, around everything that touches the bench's checkout,
build or service. `build/ci/gate-lock.ps1` implements it:

- **To take the lock**, create the file exclusively (it must not already exist)
  with one line: `<holder> <UTC time>`. The holder is one word. The job's is
  `gitlab-pipeline-<pipeline id>-job-<job id>`.
- **To release it**, remove the file, but only when its line starts with your
  holder.
- **A stale lock**, older than five hours, is taken over, and the takeover is
  logged. Five hours is one hour more than the longest a job may run.
- The job waits up to 90 minutes for a lock that another holder has, and then
  gives up with exit 10, a warning (13 for a `v*` tag).

The split gate takes the same lock with the same script. Copy the script to
the bench next to `remote-gate.ps1`, and put it around the remote half:

    scp build/ci/gate-lock.ps1 administrator@192.168.11.150:C:/bench/
    ssh administrator@192.168.11.150 "pwsh -NoProfile -File C:\bench\gate-lock.ps1 -Take -Holder split-gate-<tag>-<sha> -WaitMinutes 0"
    ...the remote half...
    ssh administrator@192.168.11.150 "pwsh -NoProfile -File C:\bench\gate-lock.ps1 -Release -Holder split-gate-<tag>-<sha>"

`-Take` exits 0 once the lock is yours, 1 when the bench is busy, and 3 when the
lock cannot be made. `-Release` exits 0 when it removed your lock, 4 when the
lock is someone else's (and it leaves that lock alone), and 5 when there was no
lock.

From GitHub Actions to GitLab CI
--------------------------------

Every job of the old `.github/workflows`, and where it went. The workflows were
removed from the tree when the project moved to GitLab. They are in the git
history, unchanged from `faca023d4` until `6593259e0` removed them on
26 September 2026 (its parent, `d8ba4b646`, is the last commit that has them),
and the ones that built and signed releases up to 6.3.3 at the tag `v6.3.3`.
Until 28 September 2026 this page and others named `6399e08a5`, the same
commit before its branch was rebased onto `master`; `master`'s history does
not hold it.

| GitHub workflow and job | GitLab job | Runner | When |
|---|---|---|---|
| `ci.yml` control-panel | `windows-dotnet` | bench150 | a protected ref |
| `ci.yml` tools | `windows-dotnet` | bench150 | a protected ref |
| `ci.yml` upload-coverage-csharp | not ported: it fed GitHub's code-quality page, and the tests run without the coverage collector | | |
| `style.yml` editorconfig | `editorconfig` | shared | every pipeline |
| `style.yml` license-headers | `checkers` (row 139) | shared | every pipeline |
| `style.yml` dotnet-format | `windows-dotnet` | bench150 | a protected ref |
| `server-build.yml` | `windows-gate`, which builds the Release server before the suite | bench150 | a protected ref |
| `dco.yml` | `sign-off` (row 139); since 26 September 2026 for every merge request and every `batch*` branch, the maintainers' commits included | shared | a merge request, a `batch*` branch |
| `roadmap.yml` | `roadmap` | shared | `Roadmap.md` changed |
| `linux-build.yml` stamps | `checkers` runs `check-linux-version-stamp.sh` (row 139), and `linux-build` runs it again before it packages: a drifted stamp makes no package | shared, linix | every pipeline; a protected ref |
| `linux-build.yml` build, x86-64 clang | `linux-build` (with the packages) | linix | a protected ref |
| `linux-build.yml` build, x86-64 GCC | `linux-build-gcc` (`.gitlab/ci/gcc.yml`; [The GCC build](#the-gcc-build)), since 6.3.6 | linix | `master`, a `batch*` branch |
| `linux-build.yml` build, AArch64 | not ported: see below | | |
| `linux-build.yml` build, the AppImage | the `appimage-*` jobs of `.gitlab/ci/packages-appimage.yml`, x86_64 and aarch64, with a runtime of the project's own and no linuxdeploy, `appimage-runtime-sources`, the source of the libraries linked into that runtime as Alpine built them, and `appimage-bundled-sources`, the source of the libraries the images carry in `usr/lib` as Ubuntu built them ([PackagesAppImage.md](PackagesAppImage.md)); carried by a release from 6.3.6 | linix | a `v*` tag and a `batch*` branch; by hand on `master` |
| `linux-build.yml` cross | `linux-cross` | linix | a protected ref |
| `linux-build.yml` package-install | `linux-package-deb`, `linux-package-rpm` | linix | a protected ref |
| `linux-build.yml` regression-on-linux | `linux-suite` (row 139), the whole suite in one run | linix | a protected ref |
| `linux-build.yml` db-upgrade | `linux-db-upgrade-pgsql`, `linux-db-upgrade-mysql` | linix | a protected ref |
| `linux-build.yml` image | `linux-image` and `linux-image-arm64`: linux/amd64 and linux/arm64 built and inspected, not pushed; for a `v*` tag, `container-image` pushes both under one index and `container-image-sign` signs the index and each image | linix, shared | a protected ref; a `v*` tag |
| `linux-build.yml` publish | `release`, `release-sign` and `release-publish`: the `.deb`, the `.rpm` and their sums on the release | shared, linix | a `v*` tag |
| `packaging.yml` chart-and-compose | `chart-and-compose` | shared | the chart or the Compose file changed |
| `packaging.yml` update-helper | `update-helper` | shared | the helper, its test or a unit changed |
| `package-managers.yml` templates | `package-templates` | shared | the templates or their scripts changed |
| `package-managers.yml` manifests | not ported: it renders from a published, signed installer and submits it; still the release-chain row's (137) | | |
| `sbom.yml` | `linux-sbom`: made and kept; a `v*` tag's release carries them, signed | linix | a protected ref |
| `fuzz-nightly.yml` | `linux-fuzz`, on Linux rather than Windows | linix | the fuzz schedule |
| (none) | `linux-sanitize`: `snmp-tests` under AddressSanitizer, once the SNMP row adds `HM_SANITIZE` | linix | a protected ref |
| (none) | `linux-repro`: the binary, the `.deb` and the `.rpm` built a second time and required to be `linux-build`'s, byte for byte | linix | a protected ref |
| `codeql.yml` | not ported: see below | | |
| `dependency-review.yml` | `mr-dependency-check`, against OSV's own export rather than GitHub's advisory service, since 6.3.6 ([Change checks](#change-checks)) | shared | every change: a merge request, and a `batch*` branch |
| `scorecard.yml` | `repo-hygiene`'s six checks on every protected pipeline, and Scorecard itself from cache.nixos.org in `scorecard`, weekly, published on the project's own site since 6.3.6 ([OpenSSF Scorecard](#openssf-scorecard)) | linix | a protected ref; the `scorecard` schedule |
| `upstream-watch.yml` | not ported: see below | | |
| `sign-release.yml` | `release-authenticode`, `release-sign` and `release-verify`: Authenticode with a federated credential in place of a client secret, and Sigstore; not the SLSA provenance (see below) | bench150, shared, linix | a `v*` tag |
| `verify-binary-provenance.yml` | `checkers` runs `build/check-binary-manifest.py`, its tree half, since 6.3.6 ([Change checks](#change-checks)); its Windows job, the fetched files' Authenticode signatures, is `build-inputs-authenticode` on bench150 since 6.3.6 | shared | every pipeline |
| `installer-smoke.yml` | `build/ci/installer-smoke-run.sh` from the VMware host, and a placeholder `installer-smoke` job ([The installer smoke test](#the-installer-smoke-test)) | the VMware host, not a runner | by hand, before a release is published |

**Not ported, and why:**

- `codeql.yml`. The CodeQL CLI and its query packs are GitHub's, downloaded
  from GitHub, and its results went to GitHub's code scanning. There is no
  other source for either.
- `dependency-review.yml` and `scorecard.yml`, as they were. Both are GitHub
  services: the first reads GitHub's advisory database for a pull request's
  dependency changes, the second scores the repository through GitHub's API and
  publishes to the OpenSSF API from a verified GitHub workflow. What each did
  is done another way since 6.3.6 (the table above).
- `upstream-watch.yml`. It reads upstream hMailServer's repository, which is on
  GitHub, and files an issue there.
- `package-managers.yml` manifests, and the SLSA provenance and the `.sigstore.json` copies of the
  bundles that `sign-release.yml` made. The SLSA generator is a GitHub reusable
  workflow with no GitLab form, and the copies were for OpenSSF Scorecard, which
  reads GitHub. They stay with the Roadmap2 row "The release chain off a single
  forge" (137); the signing and publishing it built are
  [The release](#the-release).
- `ci.yml`'s coverage upload. It went to GitHub's code-quality page. The tests
  still run; the coverage is not collected.
- The native AArch64 build of `linux-build.yml`'s matrix. linix is one x86-64
  machine; `linux-cross` compiles every file for AArch64. The GCC build of the
  same matrix was left out at the port for the time a second whole build adds to
  linix's queue (about twelve minutes). The Debian and Ubuntu package jobs now
  build with those releases' GCC ([The GCC build](#the-gcc-build)), linix
  has taken three jobs at once since 28 September 2026 ([linix](#linix)), and from 6.3.6
  `linux-build-gcc` builds with the CI image's GCC 15, on `master` and `batch*`
  pipelines only.

Where the tools come from
-------------------------

Nothing is downloaded from GitHub, at build time or at run time. Every image is
pinned by digest where the CI file names it.

| Job | Image | Tools, and where each comes from |
|---|---|---|
| `editorconfig` | `alpine:3.24` (Docker Hub) | editorconfig-checker 3.0.3, Alpine's community package. Its own releases are on GitHub, and its npm and PyPI packages download them from there when installed. The old job pinned 3.11.3; 3.0.3 reads the configuration only when named (`-config .editorconfig-checker.json`) |
| `chart-and-compose` | `alpine:3.24` | Helm 3.19 and Docker Compose 5.1, Alpine's community packages |
| `update-helper` | `ubuntu:26.04` (Docker Hub) | `systemd-analyze`, Ubuntu's `systemd` package; dash is the image's `/bin/sh` |
| `package-templates`, `roadmap` | `mcr.microsoft.com/dotnet/sdk:10.0` (Microsoft) | `pwsh`, in Microsoft's SDK image; PyYAML and jsonschema, Ubuntu's packages |
| `linux-build`, `linux-build-gcc`, `linux-suite`, `linux-cross`, `linux-sanitize`, `linux-db-upgrade-*`, `linux-sbom`, `linux-repro`, `linux-fuzz` | `registry.gitlab.com/hmailserver/hmailserver/ci-linux`, by digest (built on linix) | Ubuntu's packages; the .NET SDK and PowerShell from Microsoft's SDK image; Syft 1.52.0 from `anchore/syft` on Docker Hub ([The image](#the-image)) |
| `semgrep-sast` | `registry.gitlab.com/hmailserver/hmailserver/mirror/semgrep`, by digest: GitLab's `security-products/semgrep` 6.26.1, copied digest for digest ([Change checks](#change-checks)) | the analyzer, Semgrep and GitLab's rules, all in the image |
| `secret_detection` | `registry.gitlab.com/hmailserver/hmailserver/mirror/secrets`, by digest: GitLab's `security-products/secrets` 7.40.0, copied digest for digest ([Change checks](#change-checks)) | the analyzer, gitleaks 8.30.1 and GitLab's rules 0.25.1, all in the image; `build/scan-secrets.py` takes the same image |
| `image-pins` | `python:3.14-slim-trixie` (Docker Hub) | Python's own library alone; it asks each pin's own registry for its manifest, and downloads nothing |
| `container_scanning`, `container_scanning_arm64` | `aquasec/trivy:0.74.0`, pinned by digest (Docker Hub): `dependency-scan`'s ([Security scanning](#security-scanning)) | Trivy 0.74.0; its vulnerability database, that day's, from `mirror.gcr.io`, with `public.ecr.aws` as the fallback; the image scanned, read from the project's registry without credentials |
| `semgrep-sast-gate`, `mr-dependency-check` | `python:3.14-slim-trixie` (Docker Hub) | git from Debian's archive; OSV's export from Google Cloud Storage (`osv-vulnerabilities.storage.googleapis.com`) and OSV's API (`api.osv.dev`); the checkers are the standard library's Python |
| `scorecard` | `nixos/nix:2.35.2` (Docker Hub) | OpenSSF Scorecard 5.5.0 and its closure from `cache.nixos.org`, by its Nix store path and pinned NAR hash, each path checked against the signature of the `cache.nixos.org-1` key the image trusts ([OpenSSF Scorecard](#openssf-scorecard)) |
| `linux-db-upgrade-*` services | `postgres:18`, `mariadb:13` (Docker Hub) | the database servers |
| `linux-package-deb` | `ubuntu:26.04` | the package's dependencies, PostgreSQL's client, curl and iproute2 from Ubuntu's archive |
| `linux-package-rpm` | `registry.fedoraproject.org/fedora:44` (Fedora) | `rpm`, in the image |
| `linux-image`, `linux-image-arm64` | `gcr.io/kaniko-project/executor:v1.24.0-debug` (Google) | kaniko, and its busybox; the image it builds installs the `.deb` from Ubuntu's archive, the arm64 one Ubuntu's arm64 packages, run under linix's qemu-user |
| `container-image`, `container-image-artifacthub` | `docker.io/regclient/regctl:v0.11.6-alpine` (Docker Hub, by digest) | regctl, the release `HM_REGCTL_IMAGE` pins for the channels, and Alpine's busybox |
| `windows-dotnet` | bench150's own | the .NET SDK, Visual Studio's Build Tools and the .NET Framework SDK, from Microsoft; NuGet packages from nuget.org, held to the lock files |

### Nothing from GitHub, in the job

The runners are to enforce the rule as well as follow it: the runner entries of
linix and server2 are to map `github.com`, `api.github.com`, `codeload.github.com`,
`objects.githubusercontent.com`, `raw.githubusercontent.com` and `ghcr.io` to
0.0.0.0 in every job's `/etc/hosts` (`extra_hosts`), and bench150's hosts file
to do the same, so that a tool that quietly reaches for GitHub fails by name.
bench150's does since 29 September 2026, at 0.0.0.0 and at `::`
([bench150](#bench150)).
Two things that would not cover were found on 28 September 2026:

- **An image with no `/etc/nsswitch.conf`**, such as Docker Hub's `nixos/nix`.
  glibc's default for a missing file is `dns [!UNAVAIL=return] files`: curl,
  git and nix itself in such an image ask DNS first and read the hosts file only
  when DNS is down. (Go's own resolver, as in Scorecard, reads the file first
  either way.)
- **IPv6.** A hosts file that maps a name to 0.0.0.0 alone answers an IPv6
  lookup with nothing, and the C library then asks DNS, whose AAAA answer for
  `objects.githubusercontent.com` or `raw.githubusercontent.com` is GitHub's.
  With a canary name at 0.0.0.0 alone, `curl -6` went to the name's real IPv6
  address; with `::` beside it, to `::`. The runner's entries want `::` too.

And a job on GitLab's hosted runners - a merge request's checks - has no entries
at all, nor, while the entries above are still to be made, a job on linix or
server2: neither runner's `config.toml` maps a GitHub host (read 9 October 2026;
linix's `extra_hosts` holds one entry, for `archives.boost.io`). So a job can take
the rule into itself: `.gitlab/ci/no-github.yml` defines `.no-github`, whose
`before_script` adds the six hosts at 0.0.0.0 and at `::` to the job's own
`/etc/hosts`, writes `hosts: files dns` where there is no `/etc/nsswitch.conf`,
and fails the job, saying why, where the file asks anything before the hosts
file or the job cannot write either (an image that does not run as root). A job
with no `before_script` of its own takes it with `extends: .no-github`; a job
with one begins it with `- !reference [.no-github, before_script]`, since a
job's own `before_script` replaces whatever `extends` brings. It is plain POSIX
`sh` with built-ins only, and was run on 28 September 2026 in `nixos/nix`,
`python:3.13-slim-trixie` and `alpine:3.24` (each passed), in Debian with
`hosts: dns files` and as a user who cannot write `/etc/hosts` (each refused).
`scorecard`, `semgrep-sast-gate` and `mr-dependency-check` take it, and so do
the AppImages' two sources jobs, `appimage-runtime-sources` and
`appimage-bundled-sources`, which fetch the libraries' sources from Alpine's and
Ubuntu's archives and never from the address a recipe names (GitHub, for five
of the Alpine ones): `build/check-release-chain.py` fails when either stops
running `.no-github`'s `before_script`.

The .NET job
------------

`windows-dotnet` is `ci.yml`'s two jobs and `style.yml`'s `dotnet format` in
one, on bench150, run by `build/ci/windows-dotnet.ps1`: the settings search
index is what `generate-settings-index.ps1` writes; the COM wrapper made from
the IDL alone (`generate-com-wrapper.ps1 -FromIdl`); the Control Panel restored
against its lock file, built with warnings as errors, published and tested; the
tools' solution likewise, the Import Tool's tests, DBSetup's tests, and `build-tools.ps1`'s
publish of every tool the installer ships; then `dotnet format
--verify-no-changes` over both. Every step runs unless it needs what an earlier
one failed to make, and the summary names every failure.

It works in the runner's own checkout of the commit, not in the bench's, and
touches neither the bench's build nor its service. It takes the bench's gate
lock all the same, waiting up to 200 minutes for it (`HM_GATE_LOCK_WAIT_MINUTES`):
a .NET build beside a running suite takes the CPU the suite's timing-sensitive
tests expect, and the maintainer's split gate runs a suite over SSH that GitLab
knows nothing about. Its exit codes are the script's (0 passed, 1 a step
failed), and 10 when the lock stayed another holder's.

The packages and the image
--------------------------

`linux-build` makes the `.deb` and the `.rpm` with CPack from the build it has
just made, after checking the version stamps, and keeps them in its artifact
(`linux-build/packages/`, with their SHA-256 sums). A packaging failure fails
`linux-build` with exit code 4, its log ending `PACKAGING FAILED`, once its tester
programs have run (`build/ci/linux-build.sh`; 1 when a tester failed as well);
until 2 October 2026 the script exited 0 and the job was green with no packages
in its artifact. The package jobs, `linux-image` and `linux-repro` fail on the
missing files and point at `linux-build/package.log`. `linux-build` takes no
`allow_failure` for that code: a pipeline that made no packages fails, and the jobs
after it do not run (`build/check-release-chain.py` refuses an allowed exit code on a
job the release waits for unless the code can never come from a tag's pipeline).

`linux-package-deb` installs the `.deb` in a clean Ubuntu 26.04 container with
`apt`, asserts every file a working installation needs, configures the packaged
INI against a PostgreSQL service the way the packaging README tells an
administrator to, and runs the server's own verbs as the service user. Then it
starts the server as its unit would - as `hmailserver`, in `/var/lib/hmailserver`,
with `CAP_NET_BIND_SERVICE` alone and no new privileges, through `setpriv` - and
asks whether the REST API answers and whether `GET /` is the Control Deck, byte
for byte. What a container cannot show is the start under systemd itself, with
the unit's sandbox in force: the old job's hosted runner had systemd, and a job
container does not. `package-systemd-ubuntu26.04` and `package-systemd-fedora44`
show it, on linix's privileged runner entry: the `.deb` and Fedora 44's `.rpm`
started under their unit with systemd as PID 1 (PackagesDeb.md, "Started under
systemd"). `update-helper` still verifies the units.
`linux-package-rpm` installs the `.rpm` in Fedora 44 with `--nodeps`, as before:
it is built on Ubuntu, so its requirements name Ubuntu's sonames. From 6.3.6 it
first reads that the package requires glibc's character-set converters
(PackagesRpm.md, "The character sets"), which `--nodeps` then leaves out.

The package is built on Ubuntu 26.04 now, where the GitHub runner built it on
24.04; its `Depends` asks for glibc 2.38 or later, which 24.04 has, and the
container image, whose base is still Ubuntu 24.04, installs it.

`linux-image` builds `platform/packaging/docker/Dockerfile` for linux/amd64 from
that `.deb` with kaniko, which needs no Docker daemon and no privilege, into a
tarball, and checks what it built: the platform, the user, the entrypoint, the
healthcheck, the nine ports, and the file capability `setcap` gave
`/usr/bin/hmailserver`, read from the layer. `linux-image-arm64` builds the same
Dockerfile for linux/arm64 from `deb-ubuntu26.04-arm64`'s `.deb` and checks the
same. The Dockerfile takes the `.deb` of the architecture it is built for
(`TARGETARCH`, which `linux-image.sh` passes to kaniko with `--build-arg` beside
`--custom-platform`, since kaniko does not set it from that flag), and its `RUN`
ends with `hmailserver --version`, so the server runs on the image's platform, with the
image's libraries, during the build: for arm64 on linix, an x86-64 machine,
through its qemu-aarch64 binfmt handler, whose F flag lets kaniko's
unprivileged container run arm64 programs without the emulator in it (162 to
217 s on 29 September 2026, beside a pipeline; the amd64 build 44 to 50 s). Before
it builds for arm64, `linux-image.sh` runs `build/ci/arm64-emulation.sh`, which
refuses by name when that handler is gone (PackagesDeb.md, "What is left"). Neither job
runs the image; that needs a daemon. The tarballs are not kept: for a `v*` tag,
`container-image-amd64` and `container-image-arm64` build the images again from
the `.deb` files the release carries, and `container-image` pushes both
([The container image](#the-container-image)).

The fuzz schedule
-----------------

`linux-fuzz` is the old nightly fuzz workflow, moved from a hosted Windows runner
with clang-cl to linix with clang: the same three MIME harnesses under `fuzz/`,
the same dictionary, the same reproducers and the same libFuzzer flags, built by
`build/ci/linux-fuzz.sh` (`fuzz/build-fuzz.ps1` stays the way to build them on
Windows). It replays every kept input and every committed reproducer first, then
fuzzes each harness for `HM_FUZZ_MINUTES` (20) with `HM_FUZZ_JOBS` workers (2),
on the corpus the earlier nights grew, which the runner's cache carries from one
night to the next - the cache of the place the job runs in, one of linix's three
([linix](#linix)) or of server2's ([server2](#server2)), so two nights in
different places grow two corpora. A finding
fails the job. The job's artifact is
`fuzz-results/` - each harness's log and, on a finding, the input libFuzzer
wrote - kept whether the job passed or failed, for 30 days, and readable by the
project's Developers and above only (`access: developer`): the project's jobs are
public, and an input that crashes the MIME parser may work as an exploit
against every server not yet fixed. For the same reason the job log, which
anyone can read, names a finding and its kind and no more: the sanitizer's
report, the place and the input (libFuzzer prints one of up to 256 bytes in
full) are in the artifact only. [Fuzzing.md](Fuzzing.md) says what to do
with one.

It runs in a pipeline of its own and nowhere else: its rule asks for a
scheduled pipeline on a protected ref whose schedule's description is `fuzz`,
and every other job refuses it (`pages` by that description, the rest because
it is a schedule's). **Creating the schedule is a one-time step for the owner**,
in the project's *Build*, *Pipeline schedules*, *New schedule*:

- description exactly `fuzz` - the rule compares the whole description, so
  nothing else goes in the field;
- interval pattern `10 2 * * *` (custom), time zone UTC - 02:10, after the
  day's pushes, as before;
- target branch `master`;
- no variables: the project refuses them on a schedule ([Which pipeline runs
  what](#which-pipeline-runs-what)). The time box - `HM_FUZZ_MINUTES` (20; three
  harnesses run in turn, so keep it at 35 or less inside the job's two hours)
  and `HM_FUZZ_JOBS` (2) - is in the job in `.gitlab-ci.yml`, and a change to it
  is a commit;
- activated.

Then start the first run with the schedule's *Run* button, and check that its
pipeline holds `linux-fuzz` alone. The schedule runs as the maintainer who owns
it, who must be allowed to push to `master`, because a scheduled pipeline on a
protected branch is that user's. *Run pipeline* by hand does not start it: a
pipeline started that way is not a schedule's. It takes one of the three places
of linix or of server2 for a little over an hour, and a protected ref's
pipeline that arrives meanwhile runs beside it, on the same cores. To start the corpus again from the
seeds, clear the runner caches
(*Pipelines*, *Clear runner caches*).

The schedule was made on 27 September 2026: schedule 4461177, `fuzz`,
`10 2 * * *` in UTC, on `master`, active. By 00:40 UTC on 28 September it had
run no pipeline; its first was due at 02:10 that day. That first night,
pipeline 2887914840, held `linux-fuzz` alone and passed. By 5 October 2026 the
schedule had made eight pipelines, seven passing; the one of 1 October,
2899484982, failed before the job's script started, when linix could not
resolve `gitlab.com` to fetch the commit (the project's API, 6 October 2026).

The coverage schedule
---------------------

`linux-coverage` measures how much of the native server the tests reach, the
figure the OpenSSF criteria `test_statement_coverage80`, `test_statement_coverage90`
and `test_branch_coverage80` ask for. `build/ci/linux-coverage.sh` configures the
server with `-DHM_COVERAGE=ON` (clang's `-fprofile-instr-generate
-fcoverage-mapping`, counters written continuously so that a server stopped by
SIGKILL still leaves them), runs the `*-tests` programs, then
`build/ci/linux-suite.sh` - the whole of LinuxRegressionTests on the same
PostgreSQL 18 service - against the instrumented server, and merges the
counters with `llvm-profdata`. Its artifact, kept 30 days, is `summary.txt`
(lines, regions, branches and functions), `report.txt` (one line per file),
`coverage.json`, `lcov.info` and the suite's results; GitLab reads the line
figure from the log (`coverage:`). The vendored QuickJS and zlib are left out of
the figure and counted in `report-with-vendored.txt`.

The figure is a floor for the server as a whole: the Linux suite links the
fixtures the REST shims can drive, fewer than the Windows suite runs, and the
Linux build compiles the portable core and not the COM layer. The Windows line
figure is `build/coverage-windows.ps1`'s (OpenCppCoverage, run by hand on a bench,
elevated; it has no branch mode). A failed test does not fail the job -
`linux-suite` is the pass or fail truth, on the uninstrumented build - but a
suite that did not run to its end does (no TRX: the server did not start, or
the restore failed), and so do no counters and no report: the counters of a
suite that never ran are only the set-up's, and a figure from them would carry
the whole suite's name. `summary.txt` gives each run's tests - run, passed,
failed, skipped - beside its scope. By hand on linix, the script's header gives the two
`docker run` lines, with a PostgreSQL container on a network of its own.

**Creating the schedule is a one-time step for the owner**, as for the fuzz
schedule: description exactly `coverage`, interval pattern `40 3 * * 0`
(custom, UTC - Sunday 03:40, after the night's fuzzing), target branch `master`,
no variables, activated. The job takes linix for about three hours: the
instrumented build, then the whole suite, which with the fixtures linked on 26
September 2026 comes to some two and a half hours (*What they cost*).

What the first runs by hand found
---------------------------------

Running the ported jobs' scripts on linix on 25 September 2026 found four
things, each fixed in the same change:

- `linux-build`'s OpenPGP round trip failed in the CI image, old tag and new:
  GnuPG 2.4.8, as Ubuntu 26.04 ships it, answers "Bad passphrase" to a
  `--decrypt` in the same agent that has just imported the fixture keys, and a
  fresh agent takes the same passphrase. The script now restarts the agent after
  the import.
- The schema-upgrade gate's rewind to 6029 left behind the indexes later steps
  had made on tables 6029 already had (`idx_hm_messagetrace_queueid`, which the
  6089 step then made again), and MariaDB would not drop a column a unique index
  still named. The rewind drops each step's indexes, and on MariaDB a column's
  indexes before the column.
- With the rewind right, the MySQL upgrade stopped at 6085: the 6086 step drops
  `idx_hm_calendarshares_pair` to widen it, InnoDB refuses because the calendar
  foreign key uses it (ERROR 1553, which the step ignores as marked), and the
  `CREATE UNIQUE INDEX` of the same name then fails. That is every MySQL or
  MariaDB database upgraded through 6086, not only the gate's; the same
  statements fail on MySQL 5.7 and 8.4 and MariaDB 10.6 and 11.8. The step now
  makes the widened index under another name first, drops the old one - which
  the foreign key no longer needs - and renames the new one.
- The first Linux fuzz replay: see [Fuzzing.md](Fuzzing.md), "The nightly job".

What the first pipeline found
-----------------------------

The first pipeline, 2884863496, ran on `master` (40c7d3e0c) on 26 September
2026. Eleven jobs failed, each on a fault of the job, its script or a test.
Mending them turned up three small faults of the server's own, fixed as
well: two that reading the static analysis's findings led to, and one the
upgrade gate's new comparison found. What each was, and what fixes it:

- `update-helper`: GitLab's shared runners, where the job ran until 9 October
  2026, check out with umask 0000, as the docker runners of linix and server2
  do, and `systemd-analyze verify` refused the world-writable units. The job gives
  the checkout's three units the mode the package installs them with, 0644.
- `harnesses`: the portal harness took the first resize handle of the week
  view, which in UTC is another event's, and two more of its checks assumed
  the zone the run is in. They pass under sixteen zones now.
- `linux-suite`, 3 of 1,962: the CI image had no 7-Zip, which the server
  runs for every backup (two tests), and a test asked for SMTP on port 25,
  where the Linux suite's server listens on 2525. Over batch40 the Linux test
  project did not build at all: fixtures folded into it named a helper class
  that lived in a fixture the project does not link, the GeoIP fixture, and
  shim members it did not have. The helper has a file of its own, both are
  linked, and the shims carry the members - on their REST routes, or skipped
  with the reason where there is none.
- `windows-gate`, 1 of 3,698: `ThreadCommand` took the Nth message sent to
  be sequence number N, and two messages delivered on two threads were
  stored the other way round. `Deliver` now waits for each message.
- `windows-dotnet`: the settings-index test took the section in
  `ProfileApi.ReadString("Directories", ...)` for a setting.
- `linux-db-upgrade-pgsql` and `-mysql`: the gate's rewind to 6029 dropped
  the first of the two columns the 6101 step adds in one `ALTER TABLE`, not
  the second, and the upgrade stopped there. The gate now starts from the
  create script of 6029 itself, so there is no rewind to go stale, and
  compares the upgraded database with a new one. That comparison found the
  two columns the 6055 step gives a default and the create scripts did not
  (they do now), and on MySQL the text columns the utf8mb4 conversion of
  6102 leaves one size wider (MEDIUMTEXT for TEXT), which it accepts.
- `repo-hygiene`: six images named by a tag that can move, now pinned by
  digest, and Token-Permissions: the `renovate` job on linix outside
  `.own-runners`, and MariaDB's root password written in the file. The check
  now judges each job by the rules it runs by ([The repository checks](#the-repository-checks-and-the-scorecard-checks-left-out)),
  and the MariaDB service has no root password.
- `semgrep`: ten findings beyond the baseline, in shell scripts, and seven
  more over batch40, three of them in `build/generate-package-repos.py`. They
  are fixed, or suppressed where they are with the reason: a list split into
  words on purpose, the GnuPG home's owner-only mode, and XML the build has
  just written itself.
- `static-analysis`: 232 findings beyond the baseline (250 over batch40), in
  code that landed after the baseline was written: the PST import, object
  storage, SNMP, the secure defaults. Each was read, and two led to
  defects: `ServiceAccount::ProcessAccountName` called `getpwuid`, whose
  buffer is the whole process's, from several threads (it calls `getpwuid_r`
  now), and the inbound webhooks' URL check refused a control character but
  let a character outside ASCII into the request line, on Windows and Linux
  alike (it refuses it now). The finding there, a `wchar_t` read as a signed
  value, was not the fault itself - no character is negative - but reading
  the check showed the gap. The rest are false positives, or
  harmless where they are, of the kinds the baseline already held. The
  baseline is written again: 1,163 findings, the 249 new to it each with
  its reason on its line, and none of the 393 no scan reports any more,
  372 of them the `throw;` that `HM_TEST_FAILED` was until it threw an
  exception of its own on 25 September.
- `static-analysis` and `dependency-scan` could not upload their reports:
  GitLab takes one file per report type ("only one file can be sent as
  raw"). Each job now writes one report of all its tools' findings.
  `dependency-scan` had passed: none of its findings is high or critical
  with a fixed version.

The installer smoke test
------------------------

The real Windows installer is installed, upgraded and uninstalled on a
throwaway machine, and every promise it makes is checked. This replaces
GitHub's `installer-smoke.yml` (in the git history at `v6.3.3`), which ran on a hosted runner
that was thrown away after the job. Running the installer on the laptop or on
bench150 would take over their service and their COM registration, so it runs
on neither.

**Where it runs.** The test runs on a Windows Server 2025 VM, "Win server 2025
small", at 192.168.11.175, with 4 vCPUs and 8 GB of memory. The VM runs in
VMware Workstation on the maintainer's laptop, and its `.vmx` is
`C:\Users\chris\Documents\Virtual Machines\Win server 2025 small\Win server 2025 small.vmx`.
It has one snapshot, `firstsnapshot`, which was taken after SSH was enabled
and before anything else was installed. The VM is reverted to that snapshot
before every run and again after it, whether the run passed or failed. That
host is not a GitLab runner, and reverting the VM needs `vmrun` on that host.
So the `installer-smoke` job in the CI file is only a placeholder: in a
`batch*` or `v*` pipeline it is a manual button that fails with a sentence
saying where the test runs from. It becomes a real job when a runner exists on
that host. For a release, the installer it tests is the signed one in the
`release-sign` job's artifact (`release-signed/`), and it passes before
`release-publish` is started ([The release](#the-release)).

**How to run it.** Run it from Git Bash on the VMware host. The first installer
is installed, the second is installed over it as the upgrade, and then
everything is uninstalled:

    # a release: the previous release, then the new one
    bash build/ci/installer-smoke-run.sh \
       /c/Users/chris/Downloads/hmailserver/6.3.2/hMailServer-6.3.2-x64.exe \
       /c/Users/chris/Downloads/hmailserver/v6.3.3-1/hMailServer-6.3.3-x64.exe

    # a batch: the latest release, then the installer built from the batch
    bash build/ci/installer-smoke-run.sh --allow-unsigned \
       /c/Users/chris/Downloads/hmailserver/v6.3.3-1/hMailServer-6.3.3-x64.exe \
       hmailserver/installation/Output/hMailServer-6.3.3-x64.exe

With a single installer, the test installs it and uninstalls it. The script
takes these options: `--vmx`, `--snapshot`, `--mac`, `--host`, `--user`,
`--key` and `--out`, and each has an `HM_SMOKE_*` variable with the same
meaning. `--keep-vm` leaves the VM as the run left it, so that a failure can
be examined; the next run reverts it anyway.

The script does the following:

1. It checks that the `.vmx` carries the smoke VM's MAC address and that it has
   the snapshot. This comes before any revert, so a wrong `--vmx` cannot revert
   another machine.
2. It reverts the VM to the snapshot and starts it, waits until the address the
   VM's own VMware Tools report has held for 45 seconds, and waits for SSH. The
   VM resumes still holding the address it had when the snapshot was taken and
   asks VMware's DHCP server, on the VMware host, for a lease again; twice on 27
   September 2026 that server found the address answering, marked its lease
   abandoned and gave the VM another address 15 to 35 seconds after the revert,
   so the address the tools settle on is followed, and said, when it is not
   `--host`. A copy that fails is tried again the same way.
3. It checks that the address answers ARP with the same MAC. This comes before
   anything is copied, so a wrong `--host`, or an address the VM has let go,
   cannot install on bench150 or linix.
4. It copies the installers and `build/ci/installer-smoke.ps1` to `C:\smoke`,
   runs the script there under Windows PowerShell 5.1, and copies back
   `C:\smoke\logs`. That folder holds `smoke.log`, Inno's `install.log`,
   `upgrade.log` and `uninstall.log`, and any server logs.
5. It reverts the VM again. From the first revert to the last, a run of an
   install, an upgrade and an uninstall took about three and a half minutes on
   26 September 2026, a little over two of them in `installer-smoke.ps1`.

Only the SSH key is used, never a password. The administrator password given to
the installer is made on the VM and never printed. The output is one line per
check, starting PASS, FAIL, SKIP or INFO. The exit code is 0 when every check
passed, 1 when a check failed, 3 when the VM could not be identified, reverted
or reached, and 4 when the script did not run to a result.

**The switches.** The switches are the installer's own, from
`hMailServerInnoExtension.iss`. The first install gets
`/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TYPE=full /LOG=<file>` and the
administrator password, which a silent install takes with `/adminpassword=` or,
from 6.3.4, in `HM_ADMIN_PASSWORD`. The upgrade needs it too: DBUpdater
authenticates as the administrator before it moves the schema, and under
`/silent` it fails rather than asks. When the first installer is 6.3.4 or later,
the password carries a space, a double quote and backslashes before one, and goes
in `HM_ADMIN_PASSWORD` to both installers, since Inno drops every double quote
from its command line; that is the path by which the installer hands it to
DBSetupQuick and DBUpdater unchanged. An installer before 6.3.4 put the password
on DBSetupQuick's command line unquoted, so with one as the first installer the
password is letters and digits and goes with `/adminpassword=`. The upgrade gets
no `/TYPE`, because winget and Chocolatey pass none
([PackageManagers.md](PackageManagers.md)), and the test checks that Inno kept
the components already installed. Each installer and the uninstaller get ten
minutes. When one fails or runs out of time, the test prints the tail of Inno's
log. When the install fails, it also runs DBSetupQuick again, because the
database step's real error goes only to that tool's stderr.

**What it checks.** Everything that GitHub's workflow checked is still checked:
the service registered on the installed `hMailServer.exe`, the ports 25, 110
and 143, the COM object, the time bound and the kept log. The test adds these
checks:

- **Each installer.** Its Authenticode signature is valid. With
  `--allow-unsigned`, an installer with no signature at all is a SKIP, but a
  signature that is present and not valid still fails. Its SHA-256 and the
  version it claims (the version resource's ProductVersion, which Inno fills
  from `AppVersion`) are printed.
- **After the install, and again after the upgrade:**
  - the uninstall entry's DisplayVersion, the file version of `hMailServer.exe`
    and the COM object's `Version` all agree with the version the installer
    claims;
  - the service is registered on the installed binary, set to Automatic, has
    three restart actions (ConfigureServiceRecovery) and is running without
    being started by the test;
  - SMTP, POP3 and IMAP answer their banners;
  - the administrator password given to the installer authenticates;
  - the database is SQL Server Compact, its schema is the one the server
    requires, and the create script that was installed makes that same version;
  - `Software\hMailServer` InstallLocation is in the 32-bit view;
  - the ProgID, the class's LocalServer32, the type library and the AppID are
    registered on the installed binary;
  - the Start menu group exists;
  - the .NET 10 Desktop Runtime and SQL Server Compact 4.0 are installed;
  - nothing new appears in `ERROR_*.log` or in the Application event log under
    the source `hMailServer`.
- **Before the upgrade**, the test creates a domain, an account with its own
  settings, a server setting (the SMTP greeting) and a message delivered over
  SMTP, and reads the message back over POP3 and IMAP.
- **After the upgrade:**
  - the schema has moved to the newer build's;
  - there is one uninstaller, and the Control Panel is still installed;
  - the domain, the account, both settings and the message are kept, and the
    message's file is byte for byte the same;
  - POP3 and IMAP log the account on with its password and find the message;
  - the SMTP banner is the greeting that was set before the upgrade;
  - `[Database]` in `hMailServer.INI` is unchanged, and so is `[Security]`, but
    for `AdministratorPassword`, which may be written again as a PBKDF2 hash
    ([SecretsAtRest.md](SecretsAtRest.md)).
- **The secure defaults** ([SecureDefaults.md](SecureDefaults.md), "What an
  installation on a clean machine is to check"), each check named as that
  section names the property:
  - after a new installation by an installer from 6.3.4 on: the certificate
    record, its two files, the key's ACL, the certificate itself (RSA 2048, 825
    days, the host and where it came from), the four listeners with optional
    STARTTLS and that certificate, the two ranges, no mark left, the verb's exit
    0 in Inno's log, its rows on the audit trail and the setup checklist; then,
    with the account made, STARTTLS completing from 127.0.0.1 on all four, from
    the machine's own LAN address a password refused before STARTTLS and taken
    after it on 587, IMAP and POP3, mail taken on 25 without TLS and after
    STARTTLS, and the sign-ins over 127.0.0.1 as before;
  - after an upgrade to 6.3.4 or later: the verb not run by the installer, the
    ports, the ranges and the certificate records exactly as the older release
    left them, no mark, the checklist's `sign_in_no_tls` for *Internet* with no
    fix when the older release had no such defaults, and the verb run by hand
    refusing the upgraded server (exit 5) and changing nothing;
  - after the uninstall of a new installation: `Data\Certificates` kept.

  A new installation is a run with the new installer alone, so a release or a
  batch runs the test twice, each from the clean snapshot: the previous release
  then the new installer, and the new installer on its own.
- **After the uninstall:**
  - the service and its key are gone, and no process is left running;
  - every file left under the installation directory is one that the installer
    keeps on purpose;
  - the Start menu group, the uninstall entry and `Software\hMailServer` are
    gone;
  - every COM key the install registered is gone (647 for 6.3.3);
  - the event log source and any scheduled task naming hMailServer are gone.
    When the server wrote no event during the run, the test registers the
    source first, as the server does, so that its removal is still checked;
  - no registry key naming hMailServer is left that the clean machine did not
    have. Other new keys are listed as INFO: they belong to the .NET runtime
    and SQL Server Compact, which the installer installs when they are missing
    and never removes.

**What the uninstall keeps.** It keeps what the `.iss` keeps, and nothing else:

- `Bin\hMailServer.INI`, because its `[INI]` entries carry no
  `uninsdeleteentry`;
- the event and addon scripts under `Events` and `Addons`, which are marked
  `uninsneveruninstall`, apart from the two tools' folders under `Addons`;
- `Database`, `Data` and `Logs`, because the database tools and the server
  wrote their contents, not the installer, and `[Dirs]` removes a folder only
  when it is empty. `Data` holds `Certificates` on a new installation from
  6.3.4 on ([SecureDefaults.md](SecureDefaults.md));
- `Bin\hMailServerApiKeys.ini`, the REST API's key store beside
  `hMailServer.INI`, for the same reason: the server writes it, and
  `/GrantServiceAccess` makes it empty when a new installation's service moves
  to `NT SERVICE\hMailServer` ([ServiceAccount.md](ServiceAccount.md)). An
  installation whose service stays LocalSystem has none, which is why no run
  before the new installation of 6.3.4 on 27 September 2026 met it.

The database file, the message (unchanged) and every event and addon script
must still be there, and after a new installation the certificate and its key.
Any other file left under the installation directory fails the check.

**What it found on 25 September 2026.** Two runs of 6.3.2 then 6.3.3 with the
finished script gave the same result both times: 60 checks passed and 5
failed. Install took 24 to 26 seconds, the upgrade 8 to 10 seconds and the
uninstall 1 second. Apart from item 1 below, every install and upgrade check
passed, including the data and settings that the upgrade has to keep. The five
failures were the product's, not the test's:

1. *The upgrade writes a Critical error to the Application event log.* During
   the upgrade, DBUpdater's COM activation starts the new server against the
   old schema. The server reports HM5011, "The database is too old", as event
   2010, before DBUpdater moves the schema from 6038 to 6040. Every silent
   upgrade across a schema step does this, and the write also registers the
   event source that item 3 is about.
2. *The uninstall leaves `hMailServer.exe` and six DLLs in `Bin`:
   `libcrypto-4-x64.dll`, `libssl-4-x64.dll`, `libpq.dll`, `msvcp140.dll`,
   `vcruntime140.dll` and `vcruntime140_1.dll`.* Inno's log records "Failed to
   delete the file; it may be in use (5)", then "Removed all? No". Nothing is
   scheduled for removal at the next restart. `section_uninstallrun.iss` stops
   the service with `net stop`, which returns when the service reports
   STOPPED. The process then lives about 1.1 seconds longer: four stops
   measured 1,091 to 1,128 ms, with `net stop` itself taking 63 to 114 ms. The
   uninstaller starts deleting about 0.7 seconds after `net stop` returns.
   When the service is stopped and its process has exited before the uninstall
   starts, the log says "Removed all? Yes".
3. *The event log source is left behind.* The server creates
   `HKLM\SYSTEM\CurrentControlSet\Services\EventLog\Application\hMailServer`
   the first time it writes to the Application log (item 1). The uninstaller
   never removes it.
4. *After an upgrade, the uninstall leaves `HKCR\AppID\{5EDEC473-39E0-43F6-A234-1947071721C8}`,
   holding `LocalService=hMailServer`, in both registry views.* The
   `[UninstallRun]` entries have no `RunOnceId`, so every install recorded
   them. After one upgrade they run twice, and so does `hMailServer.exe
   /Unregister`. `hMailServer.exe` calls `RegisterAppID()` before it reads its
   command line, so the second `/Unregister`, which finds nothing registered,
   writes the AppID again and leaves it there. Running `/Unregister` by hand
   on a machine with nothing registered does the same. An uninstall without an
   earlier upgrade removes the AppID.
5. The registry comparison with the clean machine finds the key from item 3.
   This is the same leftover, found a second way.

A run of 6.3.3 on its own, with no upgrade, gave 37 passed and 1 failed. The
one failure was item 2. With no upgrade, the event log source is never
created and the AppID is removed.

**What was done about them.** All five are fixed in the tree after 6.3.3. The
proof is the smoke test of an installer built from that tree, installed over
6.3.3 with `--allow-unsigned`: every check should pass, and the unsigned
installer's signature is a SKIP.

1. The installer starts the service for the database step of an upgrade
   itself, with `sc start hMailServer --for-database-upgrade`, before it runs
   DBSetupQuick. `ServiceMain` reads the argument. When that start finds the
   database older than the binary, it refuses as before, and DBUpdater reads
   the refusal from `Connect()` as it always has, but HM5011 is not reported.
   Inno's log says the installer made that start. Only that one start is
   marked. A start by the service controller at boot, by `net start`, by
   services.msc or by a COM activation still reports HM5011, and so does a
   database newer than the binary.
2. The uninstaller stops the service at `usUninstall`, which comes before
   `[UninstallRun]` and before any file is deleted, and it waits for the
   process to exit. The stop gets `ShutdownDrainSeconds` plus 90 seconds - the
   minute the service's stop gives the message indexer's thread, and 30 seconds
   of headroom - and the process gets 30 seconds more. The upgrade's stop at the
   Ready page does the same. Every file the server's process loads is marked
   `uninsrestartdelete`, so a file still in use when the wait ends is queued
   for deletion at the next restart, and the uninstaller says so.
3. `hMailServer.exe /Unregister` removes the event source, and does it last.
   The server still creates the source at its first event, so the key belongs
   to the server from that event until the uninstall.
4. Every `[UninstallRun]` entry has a `RunOnceId`. `/Unregister` is now decided
   before `RegisterAppID()` runs, so it creates nothing, and it removes the
   AppID even when the type library was already gone. An uninstall log written
   by an earlier installer still runs that installer's entries once more,
   which is now harmless.
5. This one goes with item 3.

**The rehearsal of 26 September 2026.** Before the 6.3.4 run, the whole
harness was run with the 6.3.3 installer as both the first installer and the
"upgrade", which proves every step but the new binary: the reverts, SSH, the
copies, a same-version reinstall over the seeded data, the uninstall and the
log collection. It found two defects in the test. First, the check that no
binary is left used
`Get-ChildItem -LiteralPath ... -Include '*.exe', '*.dll', '*.tlb'`, and
Windows PowerShell 5.1 ignores `-Include` beside `-LiteralPath`, so every
file the uninstall keeps on purpose (the INI, the database, the message and the
scripts) was reported as a binary left behind. The 25 September runs never
reached that line, because the check before it failed on item 2's real
leftovers, so it would first have shown itself on the 6.3.4 run, as a failure
of the fixed uninstaller. It now filters by extension. Second, one run stopped
before the install with "Connection reset by peer" from the first `scp`, after
the VM had answered SSH and within 81 seconds of the revert, and the harness
tried nothing again. Making `C:\smoke`, the three copies to the VM and the
copy of the logs back are now tried up to three times, waiting for SSH between
tries; the smoke script itself is never run twice. With both fixes the
rehearsal gave 62 passed and 3 failed in 219 seconds, and the three failures
are 6.3.3's own items 3, 4 and 5: the "upgrade" runs `[UninstallRun]` twice,
and the test registers the event source. Item 2 did not appear in these runs;
it is a race, and it is fixed in the tree anyway.

**The secure defaults' checks, 27 September 2026.** The checks of
[SecureDefaults.md](SecureDefaults.md) were added and run against the released
6.3.4 installer as signed: 6.3.4 on its own gave 59 passed and 0 failed, and
6.3.3 then 6.3.4 gave 73 passed and 0 failed. They found no defect in the
product. The first new installation failed one older check, not one of them:
the uninstall left `Bin\hMailServerApiKeys.ini`, which the list of what the
uninstall keeps now has (above). Two other runs that day stopped before
installing anything, because the VM's DHCP lease moved after the revert
(step 2 of the script, above); the script now follows it.

`build/check-installer-scripts.py` holds each fix in the text of the scripts and
the server, and CI runs it on every push. The installer's own comments had the
.NET runtime's order backwards, and they are corrected too: Inno runs a `[Run]`
entry that is not `postinstall` before `ssPostInstall`. So the runtime is
installed from `section_run.iss`, and `InstallDotNetRuntime` at `ssPostInstall`
is the second attempt.

The release
-----------

A protected `v*` tag's pipeline ends in the release, for 6.3.5 onwards. It was
written on 26 September 2026, while 6.3.4 was released by hand, and **first ran
for 6.3.5**: tag pipeline 2892097590 passed every gate, `release` was started
at 09:39 UTC on 29 September 2026, the four jobs after it passed within three
minutes, and `release-publish` created the release at 10:20:45 UTC, after the
installer smoke test of the signed installer. Two things did not work, and 6.3.6
changes both: the links' paths ([What GitLab and Pages take](#the-release),
below) and `update-feed`, the trigger that was to deploy the feed, which GitLab
refused at once ([The update feed](#the-update-feed)); the tag's pipeline
therefore reads "failed". See [What waits for the owner](#what-waits-for-the-owner)
and [What 6.3.5's run showed](#what-635s-run-showed). RELEASE.md has the
runbook.

| Job | Where | What it does |
|---|---|---|
| `windows-installer` | bench150, in `C:\bench\hmailserver` under the gate lock, after `windows-gate` | Refuses a checkout with a change of its own to a tracked file, with a file git neither tracks nor ignores under `hmailserver\source` (an SDK-style project builds every source file below its folder, tracked or not), or with an untracked `Directory.Build.props`, `NuGet.Config` or kin that every build would read, in the checkout or above it; empties every folder the installer script takes whole from the checkout, and every tool's publish folder, of what git does not track; then builds the server, the tools and the Control Panel, fetches the .NET runtime and the third-party files (each against its pinned hash), refuses if a build changed a tracked file, runs ISCC, and requires the file to be `hMailServer-<version>-x64.exe` with the version resource `<version>.0` (`build/ci/windows-installer.ps1`). Automatic: it signs and sends nothing |
| `release` | linix, by hand, after every gate | Refuses unless the tag is an annotated tag that still points at the commit the pipeline built, that commit is on `master`, and the tag is signed by the one key `master`'s `.gitlab/allowed_signers` lists (fetched by name, never the tagged tree's copy; every tag is refused while that list holds more than one key), `Version.h` says the tag's version, the security scans of the commit finished and every finding they raised that `SECURITY.md`'s policy counts is in a baseline or accepted in `.gitlab/security-exceptions.yml` ([Before a release](#before-a-release-the-scans-hold-it)), `HM_COSIGN_IMAGE` is `gcr.io/projectsigstore/cosign` pinned by digest (checked as the job receives it, so a CI/CD variable of that name cannot put another image in `release-sign`) and the five Azure settings are set; then collects the x64 installer, `windows-installer`'s, and from 6.3.6 the ARM64 one, `windows-packages-arm64`'s, held to the `SHA256SUMS` that job wrote; the x86-64 `.deb` and `.rpm`, `linux-build`'s, and the generic AArch64 ones, `deb-ubuntu26.04-arm64`'s and `rpm-fedora44-aarch64`'s (from 6.3.6), each with its own SBOM (`<package>.cdx.json`, which `linux-build` or the package's SBOM job made and which must describe the package, by SHA-256); from 6.3.6 every distribution release's `.deb` and `.rpm`, both AppImages and the source tarball, each with its SBOM, and the source of the libraries linked into the AppImages' runtime, which has none, as `release-packages` gathered them, and the Helm chart, `helm-chart-package`'s; the sums as `hmailserver-linux-<version>-SHA256SUMS.txt`, `linux-build`'s `SHA256SUMS.txt` as it is with a line added for every other Linux file and the chart; and the tree's two SBOMs, checking every package, AppImage, the runtime's sources and the chart against the sums its build job wrote and the tarball with `build/check-source-tarball.py`; refuses when the package `hmailserver/<version>` already holds one of those files with other bytes, or an installer, its SBOM, a bundle, a bundle's `.sigstore.json` copy or the provenance at all, which only the jobs after it make; and last counts what GitLab and Pages will take of the release, refusing past a limit ([What a release carries](#the-release); `build/ci/release-chain.py collect`) |
| `release-authenticode` | bench150 | Signs the x64 installer into `release-authenticode/`, and, when that is signed, the ARM64 one into `release-authenticode-arm64/`, and, when that is, the PowerShell module's `HmailServer.psd1` and `HmailServer.psm1` into `release-authenticode-module/`, each through Azure Artifact Signing, and requires a valid signature with an RFC 3161 timestamp on each, by the publisher, chain and code-signing use every installation's Control Panel is pinned to, and each module file to be its unsigned bytes with one signature block appended (`build/ci/windows-authenticode.ps1`, called once for each; `-Check` holds a downloaded file to the same) |
| `release-sbom` | linix, in Python's image | Makes each installer's own SBOM, `hMailServer-<version>-x64.exe.cdx.json` and `hMailServer-<version>-arm64.exe.cdx.json`, from the bytes Authenticode signed, with `linux-sbom`'s component list for that processor (both kept by `release` in `release-tools/`), the tools' lock files and that installer's script; refuses an installer Authenticode left unchanged, and a module file that is not collect's bytes with one signature block appended; then writes the statement of the release's SLSA v1 provenance, an in-toto statement naming all sixty-two files by their SHA-256, the commit and the pipeline, for `release-sign` (`build/ci/release-chain.py sbom`, `build/asset-sbom.py`) |
| `release-sign` | linix, in cosign's `gcr.io` image by digest | Requires each installer's SBOM and the provenance's statement to name that signed installer's SHA-256, and the statement each signed module file's, then signs all sixty-two files with Sigstore, checks each bundle's format and log entry and verifies it with cosign, copies each primary file's bundle to `<file>.sigstore.json` byte for byte, attests the statement with `cosign attest-blob` (a Rekor v1 dsse entry), and finds every file in it with `cosign verify-blob-attestation` (`build/ci/release-sign.sh`) |
| `release-verify` | linix | Checks every file with `hmailserver --verify-update`, built by `linux-build` from the same commit, requires the GitLab identity, refuses a copy with a byte added, holds each compiled file's and the tarball's SBOM to the file (`build/asset-sbom.py verify`), each `.sigstore.json` copy to its bundle and the provenance's statement to the files, the commit and the pipeline (`build/ci/release-chain.py check-provenance`), and writes the notes' verification section (`build/ci/release-verify.sh`) |
| `release-publish` | linix, by hand | Refuses a release that exists already, a compiled file whose SBOM is missing or describes other bytes, a `.sigstore.json` that is not its bundle, a provenance that does not name every file, a link GitLab would refuse, a site Pages or a server would not read whole, and, before it sends anything, a file name the generic package `hmailserver/<version>` holds with other bytes; uploads the 160 files there, except any it holds with the same bytes from a run that stopped part-way, reads them back and holds every record of each name to the SHA-256 that was signed, creates the release (a Download table made from the release's files, then the version's section of `CHANGELOG.md` and the verification; [The release's notes and the order of its links](#the-release)) with a link per file named after it, the two Windows installers' made last so that GitLab lists them first, and requires the feed rendered from the API to give Windows servers on x64 and ARM64, Debian and RPM servers on x86-64 and AArch64, and the AppImages their file and bundle (`build/ci/release-chain.py publish`) |
| `pages`, `package-repos` | linix, after `release-publish` | The update feed rendered from the releases with the new one in it, the apt and dnf repositories added, and the site deployed, in the tag's own pipeline ([The update feed](#the-update-feed)). Until 6.3.6 a trigger job, `update-feed`, was to start a pipeline on `master` for this; GitLab drops a multi-project trigger into the project that runs it (`invalid_bridge_trigger`), and 6.3.5's was |

**Why two buttons.** Nothing is signed or sent until a maintainer starts
`release`, after every gate of the tag has passed. A manual job can be started
as soon as its own needs have finished, so `release` needs every job of
`.gitlab-ci.yml` that can fail the tag's pipeline, and waits for the four
security scans whose reports it reads (`static-analysis`, `semgrep`,
`dependency-scan` and `semgrep-sast`); `dependency-scan` is `allow_failure`,
`semgrep-sast` fails only when its scan does, and the first two fail only on a
finding new to their baselines. It reads
their reports and refuses on what they found
([Before a release](#before-a-release-the-scans-hold-it)). Of the package
jobs of `.gitlab/ci/packages-*.yml`, which a tag's pipeline runs too, it needs
the two whose generic files the release carries, `deb-ubuntu26.04-arm64` and
`rpm-fedora44-aarch64`, their SBOM jobs and their install jobs, which install
and run each package in an AArch64 container of its distribution under
qemu-user and so are gates; `release-packages`, which needs every other package
build and its install job, both AppImages and their test jobs,
`appimage-runtime-sources`, `appimage-bundled-sources` and the source tarball; and `helm-chart-package`. It brings the artifacts of fourteen jobs: the
ten whose files it collects (`linux-build`, `linux-sbom`, `windows-installer`,
`windows-packages-arm64`, the two package builds and their SBOM jobs,
`release-packages` and `helm-chart-package`) and the four scans.
`release-packages` is a job of its own
because GitLab.com lets a job need fifty jobs at most (`ci_needs_size_limit`):
`release` needs thirty-five, and `release-packages` forty-three, forty-one of
them jobs `release` does not need itself. Of the Windows package jobs it needs
`windows-packages-arm64`, whose ARM64 installer the release carries from 6.3.6;
a tag's pipeline has no `windows-packages-x64`.
It does not wait for `ci-image-packages` and `repo-hygiene`, which are
`allow_failure`, or for the manual `db-matrix` and `installer-smoke`: they are
not gates of a release, and `release` can start while they are still running
or after one has
failed. `build/check-release-chain.py` fails when a job of `.gitlab-ci.yml`
that can fail is not among its needs, or one that builds, describes, installs
or tests a file the release carries (through `release-packages` for the
gathered ones, and every job those wait for), when one of them may fail or
waits for a press in a tag's pipeline (`allow_failure: true` or `when:
manual`), since `release` would start after it failed, and when any job needs
more than fifty. `release-publish` waits for
the second button so that the installer smoke test can run on the signed
installer first ([The installer smoke test](#the-installer-smoke-test)):
download `release-signed/` from `release-sign`'s artifacts, run the test with
the previous release and the new installer, and again with the new installer
alone, and start `release-publish` when both pass. Both are
`allow_failure: false`, so the pipeline waits at each and the jobs after them
do not start.

**Why no secret.** Sigstore takes the job's ID token for the audience
`sigstore`; Entra ID takes another, for `api://AzureADTokenExchange`, through a
federated credential on the app registration in place of a client secret; and
the upload and the release are the job's own `CI_JOB_TOKEN`. A token lives for
the job and names the project, the tag and the pipeline.

**Whose tag.** Release tags are created by Christopher Holloway, as
`Progressiverobot`, alone (GOVERNANCE.md, 26 September 2026), and GitLab's free
tier cannot hold tag creation to one account: the protected tag rule names a
role. `release` reads `.gitlab/allowed_signers` from `master`, fetched by name
(`git show refs/remotes/origin/master:.gitlab/allowed_signers`), because a tag
on a commit of its own could carry a key of its own in its tree; it takes that
list only while it holds one key, requires the tag's signature to be that key's
by its fingerprint as well as by `git verify-tag`, and requires the tagged
commit to be an ancestor of `master`. In the job's shallow checkout, fetching
`master` brings its history down to the tagged commit when that is an
ancestor, which is all `git merge-base --is-ancestor` needs. The checks run as
the tagged commit has them, so a commit that is not `master`'s could carry a CI
file or a `release-chain.py` without them: they stop a mistake and a tag made
without the owner's key. Against a hostile commit, the protected tag rule is
what holds for the Sigstore signature every server checks, since any pipeline
of a protected `v*` tag is given a certificate naming that tag; the Azure
credential made for one tag at a time holds the installer's Authenticode
signature alone, which a server checks only with `UpdateRequireAuthenticode=1`
and never on Linux. `master`'s list is only as safe as `master`, which
Maintainers may push to.
`build/check-release-chain.py` runs the tag check against a git that answers as
the job's checkout would, and fails if it takes the tagged tree's list, a tag
off `master` or a second key.

**A file name is a release's once.** A generic package can hold a name more
than once, and a download by name is served the newest, so `release` refuses
before anything is signed when `hmailserver/<version>` holds one of the
release's files with other bytes; the installer and the bundles, which the
signing jobs make after it and which a second signing never makes the same
(a new timestamp, a new certificate), it refuses there at all. The `.deb`, the
`.rpm`, the sums and the SBOMs may be there with the same bytes, as a
`release-publish` that stopped part-way leaves them. `release-publish` refuses a
name held with other bytes before it sends anything, does not send again a
file held with the same bytes, so a retry of it finishes a publish that
stopped part-way, and after the upload holds every record of each name to its
digest. After the release is created, nothing in the pipeline stops someone
who may write to the registry adding a newer file under a release's name; a
server still refuses it without a bundle this project's tag pipeline signed.

**Why the tag's own pipeline.** Fulcio names the certificate after the ID
token's `ci_config_ref_uri`, which a child or downstream pipeline's token does
not carry, and the file's path, `.gitlab-ci.yml`, is in the identity every
server built after 6.3.3 trusts ([UpdateFeed.md](UpdateFeed.md), "What the
signing job produces"). `build/check-release-chain.py`, in the `checkers` job,
fails when a change to the file or the scripts breaks that, or the order of the
jobs, or the manual starts.

**What a release carries.** From 6.3.6, sixty-two files, a bundle beside each,
a `.sigstore.json` copy of each primary file's bundle, and the SLSA provenance:
160 files and as many links (`build/ci/release-chain.py`'s `release_files`).

| Files | How many | Made by |
|---|---|---|
| The Windows installers, `hMailServer-<v>-x64.exe` and, for Windows on ARM, `hMailServer-<v>-arm64.exe`, each the name its own processor's server looks for | 2 | `windows-installer` and `windows-packages-arm64` ([PackagesWindows.md](PackagesWindows.md)), each signed by `release-authenticode` |
| The generic `.deb` and `.rpm` for x86-64 (`hmailserver_<v>_amd64.deb`, `hmailserver-<v>-1.x86_64.rpm`) and AArch64 (`hmailserver_<v>_arm64.deb`, `hmailserver-<v>-1.aarch64.rpm`), the names an installed server's update check asks for | 4 | `linux-build`; `deb-ubuntu26.04-arm64`, cross-compiled in Ubuntu 26.04's own container; `rpm-fedora44-aarch64`, in Fedora 44's |
| Each distribution release's own package, both architectures: `hmailserver_<v>-1~<release>_<arch>.deb` for Debian 12 and 13 and Ubuntu 24.04, `hmailserver-<v>-1.<release>.<arch>.rpm` for Fedora 44 and 43, EL 10 and 9, openSUSE Leap 16.0 and Tumbleweed | 18 | `deb-<release>-<arch>` and `rpm-<release>-<arch>` (`.gitlab/ci/packages-deb.yml`, `packages-rpm.yml`), each in its release's own container |
| The AppImages, `hMailServer-<v>-x86_64.AppImage` and `-aarch64.AppImage`, with the project's own runtime | 2 | `appimage-x86_64`, `appimage-aarch64` (`.gitlab/ci/packages-appimage.yml`) |
| The source of the libraries linked into that runtime, `hmailserver-<v>-appimage-runtime-sources.tar.gz`: each library's upstream archive from Alpine's distfiles mirror and Alpine's APKBUILD and patches from aports, at the version and commit the runtime jobs recorded, each held to the SHA-512 its APKBUILD gives; the LGPL-2.1 of libfuse3 and the GPL of lzo ask for it, and each AppImage names it. Source, so no SBOM ([PackagesAppImage.md](PackagesAppImage.md), "Licences") | 1 | `appimage-runtime-sources` (`build/ci/packages/appimage-runtime-sources.sh`) |
| The source of the libraries the AppImages carry in `usr/lib`, `hmailserver-<v>-appimage-bundled-sources.tar.gz`: each Ubuntu source package (`.dsc`, upstream archives, Ubuntu's changes) at the version the image's package was built from, from Ubuntu's archive or its snapshot service, each file held to the checksums of a `Sources` index whose `InRelease` Ubuntu's archive keys signed and of its `.dsc`; the LGPL of GnuTLS, Nettle, GMP, libidn2, libunistring, libtasn1 and keyutils asks for it, and each AppImage names it in `bundled.txt`. Source, so no SBOM ([PackagesAppImage.md](PackagesAppImage.md), "Licences") | 1 | `appimage-bundled-sources` (`build/ci/packages/appimage-bundled-sources.sh`) |
| The source tarball, `hmailserver-<v>.tar.gz`, with nothing non-free in it | 1 | `source-tarball` ([SourceTarball.md](SourceTarball.md)) |
| The Helm chart, `hmailserver-chart-<v>.tgz` | 1 | `helm-chart-package` ([ContainerImage.md](ContainerImage.md)) |
| The PowerShell module, `HmailServer.psd1` and `HmailServer.psm1`, each Authenticode-signed: the tagged tree's bytes with one signature block appended, the files the PowerShell Gallery is sent ([CommandLinePackages.md](CommandLinePackages.md)). Scripts, so no SBOM, and not in the sums: Authenticode changes them after `release` writes those | 2 | `release` collects them from the tree, `release-authenticode` signs them |
| `hmailserver-linux-<v>-SHA256SUMS.txt`: `linux-build`'s two lines, then one for every other Linux file, the two AppImage sources files and the chart | 1 | `release` |
| Each compiled file's and the tarball's own SBOM, `<file>.cdx.json` (CycloneDX 1.6; SECURITY.md, "Software bills of materials") | 27 | `linux-build`, the two generic AArch64 packages' SBOM jobs, `release-packages`, and `release-sbom` for the two installers |
| The SPDX and CycloneDX SBOMs of the tree | 2 | `linux-sbom` |
| `<file>.cosign.bundle` beside each of those 62; `<file>.sigstore.json`, a byte-identical copy, beside each of the 35 but a compiled file's SBOM | 97 | `release-sign` |
| `hmailserver-<v>.intoto.sigstore.json`, the SLSA v1 build provenance: an in-toto statement naming all 62 by their SHA-256, the commit and the tag's pipeline, signed with `cosign attest-blob`. One file, named as the Sigstore bundle it is; no `.intoto.jsonl` copy, the only name Scorecard counts as provenance, which would misdescribe its format (SECURITY.md, "Build provenance"), so Scorecard counts it as a signature; `repo-hygiene`'s Signed-Releases counts it as provenance | 1 | `release-sbom` writes the statement, `release-sign` attests it |

6.3.5 carries nine files: the installer, the x86-64 `.deb` and `.rpm`, the sums,
their SBOMs and the tree's, each with a bundle. The AArch64 packages, each
distribution release's AArch64 package and the aarch64 AppImage are tested under
emulation only: their install and test jobs run each in an AArch64 container
under qemu-user on linix, and the project has no AArch64 machine. A package
job's container has neither PowerShell nor Python, so the build records what it
was built against (`built-against/`: what dpkg or the sysroot's rpm database
held, and Boost's header; an AppImage's build also what it bundles and what its runtime links) and the SBOM
is made from that in the CI image (`build/ci/packages/package-sbom.sh`), by the
generic AArch64 packages' SBOM jobs and by `release-packages`, which gathers the
rest with the tarball (`build/ci/release-chain.py gather`) and holds each to the
sums its build wrote. The ARM64 installer is `windows-packages-arm64`'s,
cross-compiled on bench150 in the runner's own clean checkout, held to the
`SHA256SUMS` that job wrote, Authenticode-signed by its own call in
`release-authenticode` and described by `release-sbom` from the ARM64 component
list; every program and library it packs was checked before ISCC ran to be ARM64
code or .NET that runs there, but Microsoft's ARM64EC `vcruntime140_1.dll`, which
the check allows; its OpenSSL, Boost and libpq come from the runner's library
cache, built once from their pinned archives and kept; and it has not been run on
ARM64 hardware
([WindowsOnArm.md](WindowsOnArm.md)). A server's update check asks for its own
processor's installer by name (`UpdateChecker.cpp`, `PlatformInstallerName_`),
so an x64 server is never offered the ARM64 one, and the site's
`/download/hMailServer-x64.exe` and `/download/hMailServer-arm64.exe` each lead
to their own file.
The provenance is the project's own, verified with cosign (SECURITY.md, "Build
provenance"): the SLSA project's generator and `slsa-verifier` are GitHub's. The
verification section `release-verify` writes into a release's notes says all of
this. The server and the tools in the installer are the bench checkout's
incremental build of the tag's commit, not a clean one, and its OpenSSL, libpq
and C++ runtime come from the bench's library tree and Visual Studio; the notes
say that too.

**The links' names and paths.** A release has one link per file. Its name is
the file's own, with the version (`hMailServer-6.3.6-x64.exe`), and that is what
a server and the update feed read. Its path (`direct_asset_path`) is the file's
name with the version taken out (`/hMailServer-x64.exe`), because GitLab serves
a link's path twice: at `/-/releases/v6.3.6/downloads/hMailServer-x64.exe`, the
release's own, and, while the release is the latest, at
`/-/releases/permalink/latest/downloads/hMailServer-x64.exe`, which is how
README.md, Channels.md and the OBS service reach the newest file without knowing
its version. From 6.3.6 every link's path is made that way, an SBOM's, a
bundle's and a bundle's `.sigstore.json` copy's included
(`/hMailServer-x64.exe.cdx.json`, `/hMailServer-x64.exe.cosign.bundle`,
`/hMailServer-x64.exe.sigstore.json`), so that the latest installer and the
bundle that verifies it are both at `permalink/latest`, under the names the
update site's `/download/` gives them ([UpdateFeed.md](UpdateFeed.md), "The
download links"). Earlier releases differ: 6.3.4's SBOMs and 6.3.5's bundles keep
the version in their paths (`/hMailServer-6.3.5-x64.exe.cosign.bundle`), and a
`permalink/latest` address of one of those names works only while that release
is the latest. GitLab's permanent link picks the release with the latest
`released_at`, a pre-release included; the update site's `/download/` names
follow the newest stable version instead. The path is the name after one slash:
GitLab puts `/-/releases/<tag>/downloads` in front of it itself. 6.3.5's links
were given `/downloads/<name>`, so GitLab served each at
`.../downloads/downloads/<name>`, a 404, until the 18 links were corrected by
hand through the API the same day; `release-chain.py` now refuses such a path
before anything is uploaded, and `build/check-release-chain.py` holds the paths,
the pairing and every `permalink/latest` address the tree writes.

**The release's notes and the order of its links.** A release's notes begin with
a `## Download` table that `release-publish` makes from the release's own files
(`release-chain.py`'s `download_table`), before the heading
`# hMailServer <version>`, the version's `CHANGELOG.md` section and the
verification. Its rows: the x64 installer, always, since a release without it
is refused before the table is made; the ARM64 installer when the release
carries it, called a preview while the tagged tree's
[WindowsOnArm.md](WindowsOnArm.md) says, in bold, "This has not been run on
ARM64 hardware."; each linked at its release-page address,
`/-/releases/<tag>/downloads` and its link's path, with the SHA-256 the job
computed of the signed file; the Linux packages, naming the release's
`hmailserver-linux-<v>-SHA256SUMS.txt`; the container image, while the project's
CI/CD variable `HM_PUBLISH_IMAGE` is `1` as the job runs, the switch the image
jobs after it read; and how to check a download, naming the provenance and the
notes' *Verification*. Every line of it is a heading, a table row or blank,
which the update feed's Atom summary passes over
(`build/generate-update-feed.py`'s `release_summary`), so a release's summary
is still its section's first paragraph. GitLab lists a release's links newest
first, under *Assets* by type, so the job makes the two installers' links last,
ARM64 then x64, and every other link before them in the order of its name: x64
and ARM64 are the first two of *Packages*. `release-chain.py --self-test` holds
the table to the files - no ARM64 row without the ARM64 installer, no preview
sentence once WindowsOnArm.md drops it, no image row while the variable is not
`1`, each SHA-256 the file's - the summary to the notes without the table, and
the order of the links. 6.3.6's notes began with its section, about 420,000
characters, and its links were all made in the order of their names, which put
the installers 24th and 25th of its 26 packages among 160 links: on
6 October 2026 the table was put above its notes and the two installers' links
made again, last, by hand.

**What GitLab and Pages take.** GitLab sets no limit on a release's links, but
each link's permanent path must be at most 128 characters of letters, digits and
`_ . - /` (6.3.6's longest is 58, and no path carries a version, so the `~` of a
distribution release's `.deb` version, which GitLab refuses there, never reaches
one), and each link's name, address and path its own.
Pages reads 1,000 `_redirects` rules and 64 KiB, and a server reads a feed of
4 MiB. `release` counts all of it before anything is signed, and
`release-publish` again before anything is uploaded, over the live releases with
the new one in them and the apt and dnf repositories kept as `package-repos`'
`--keep` keeps them, and refuses past a limit (`check_budget`). 6.3.6 alone: 154
links; `_redirects` 33 rules in 5,700 bytes, the feed's 17 and the apt and dnf
repositories' 16; `releases.json` 52,334 bytes. Each later release of its shape
adds 16 rules, 2,942 bytes and 52,331 bytes, so twenty-one such releases fit in
`_redirects` before `--keep` is needed, and some eighty in the feed
([UpdateFeed.md](UpdateFeed.md), "How much the site holds";
[PackageManagers.md](PackageManagers.md), "What the repositories hold").

### The release rehearsal

A protected `batch*` branch's pipeline builds and tests everything a tag's does,
so it also rehearses the release, publishing nothing
(`.gitlab/ci/release-rehearsal.yml`):

- `release-rehearsal`, on linix, runs `build/ci/release-chain.py
  collect --rehearse`: every file the release carries, gathered under the names
  it carries them, each held to its build's sums and its SBOM as `collect` holds
  them for a tag, and the budget above counted for this release alone. The
  version is `Version.h`'s. The x64 installer is `windows-packages-x64`'s,
  `hMailServer-<v>-<first 8 of the commit>-x64.exe`, held to that job's
  `SHA256SUMS` and collected under the release's name, because
  `windows-installer` runs for a `v*` tag alone; the ARM64 one is
  `windows-packages-arm64`'s, `hMailServer-<v>-<first 8 of the commit>-arm64.exe`,
  held and named the same way. Then `sbom --rehearse` makes the two installers'
  SBOMs and the provenance's statement.
- `release-rehearsal-sign`, in cosign's image, runs `build/ci/release-sign.sh
  --rehearse`: every file signed, each copy made and the provenance attested,
  as the branch, `https://gitlab.com/<path>//.gitlab-ci.yml@refs/heads/<branch>`,
  and each checked with cosign.
- `release-rehearsal-verify`, on linix, runs `build/ci/release-verify.sh
  --rehearse`: every SBOM, copy and the provenance held to the files, and every
  bundle read by the server's own verifier, which must refuse it at the
  identity, with "The certificate names ..., not this repository's release
  workflow". Servers trust `v*` tag identities alone, so a rehearsal's bundles
  install nowhere; that refusal is also the proof that every bundle is one a
  server reads as far as its identity.

What it cannot rehearse: the tag's own checks (an annotated tag on `master`
signed by the owner's key), the security gate, whose scans a tag's pipeline runs,
the package registry, and Authenticode, which needs the credential the owner
makes for one tag. What it touches: nothing of the project's releases or its
package registry - no job of it uses the job token GitLab gives every job, and
`release-chain.py` refuses `--rehearse` with `publish` or `gather` - and
Sigstore's public log, where each rehearsal's entries name the branch. It runs where the package jobs
run on a `batch*` branch (a push that changes documentation alone runs none),
never for a tag, `master` or a merge request; `build/check-release-chain.py`
holds all of that.

**The artifacts' lifetimes.** `linux-build`'s artifact is kept three days, so
`release` has to be started within three days of the tag's pipeline, or
`linux-build` retried first. The package jobs' artifacts are kept fourteen
days. `release` keeps what the later jobs need, the
server binary included, for thirty days, and the signed files are kept ninety.

### The container image

The image's address is `registry.gitlab.com/hmailserver/hmailserver`, the
project's path in the group `hmailserver`, and the Compose file and the Helm
chart pull it from there; 6.3.4 was built by hand and has none. It is one
multi-platform index - a Docker manifest list - naming an image for linux/amd64
and one for linux/arm64, each built from the release's `.deb` of its own
architecture; a client pulls the tag and gets its own platform's image. The jobs
of `.gitlab/ci/container-image.yml` are in every protected `v*` tag's pipeline,
after `release-publish`: a maintainer starts the first, `container-image-deb`,
by hand, and the rest follow it. They push only while the project's CI/CD
variable `HM_PUBLISH_IMAGE` is `1`, which the scripts read as the jobs run -
not a rule, which is decided when the pipeline is made - so a release tagged
before the variable is set has its image pushed from its own pipeline after
it ([ContainerImage.md](ContainerImage.md), "Publishing"). A tag's pipeline
runs the tag's own CI files: 6.3.5's made its image jobs only if the variable
was set when its pipeline was made, it was not, and 6.3.5's pipeline has none,
so the first image is 6.3.6's.

| Job | Where | What it does |
|---|---|---|
| `container-image-deb` | linix, in cosign's image (`HM_COSIGN_IMAGE`); started by hand, and the tag's pipeline shows as blocked until it is | Refuses, exit 3, unless `HM_PUBLISH_IMAGE` is `1`. Takes `hmailserver_<version>_amd64.deb` and `hmailserver_<version>_arm64.deb` and their SBOMs from the published release - not from `release-sign`'s artifact, which may have expired by then - and verifies each against its Sigstore bundle as this project's pipeline for the tag, at either of the project's two paths, with the project's id required at both (`build/ci/container-image-deb.sh`) |
| `container-image-amd64`, `container-image-arm64` | linix, in kaniko's image (`linux-image`'s, by reference) | Refuse, exit 3, unless `HM_PUBLISH_IMAGE` is `1`, and fail unless `$CI_REGISTRY_IMAGE` is the group's path. Each builds `platform/packaging/docker/Dockerfile` for its platform from the release's `.deb` of that architecture - the arm64 one under qemu-user - runs the same checks as `linux-image`, and keeps the image's tarball, its configuration and their SHA-256 for `container-image` (`build/ci/linux-image.sh`, `HM_IMAGE_RELEASE=1`, `HM_IMAGE_ARCH`). Neither pushes |
| `container-image` | linix, in regctl's image (`regclient/regctl:v0.11.6-alpine`, by digest) | Refuses, exit 3, unless `HM_PUBLISH_IMAGE` is `1`, and pushes to `$CI_REGISTRY_IMAGE` only if it is the group's path, `registry.gitlab.com/hmailserver/hmailserver` (at any other it fails and pushes nothing), with the job's own `CI_REGISTRY_USER` and `CI_REGISTRY_PASSWORD`. It holds each tarball to its job's SHA-256, each image to its architecture and the release's version, and each manifest to the configuration its job checked; joins the two under one index, a Docker manifest list, and checks that it names exactly linux/amd64 and linux/arm64, each by its digest; and only then pushes: both images by digest, and the index under the tags `:<version>`, and for a version without a suffix `:<major>.<minor>` and `:latest` too: `v6.3.6` pushes `6.3.6`, `6.3` and `latest`, and `v6.3.6-rc1` pushes `6.3.6-rc1` alone. `:<major>.<minor>` and `:latest` only move forward: each is made where there is none, moved when the image it names says (its `org.opencontainers.image.version` label) that it is this version or an older one, compared number by number, and left, with a `NOT MOVED` line in the log and in `container-image/tags.txt`, when it is a newer one or its version cannot be read ([ContainerImage.md](ContainerImage.md), "Publishing"). No tag names anything but the index. It reads every tag it wrote back, and the index's two entries, and leaves a dotenv report naming the index and each image by digest (`build/ci/container-image-push.sh`) |
| `container-image-artifacthub` | linix, in the same regctl image; in the default branch's pipeline, started by hand and allowed to fail | Refuses, exit 3, unless `HM_PUBLISH_IMAGE` is `1`, and pushes only to the group's path, only from a protected `v*` tag or the protected default branch. While `platform/packaging/docker/artifacthub-repo.yml` holds no repository ID it pushes nothing and passes; it refuses a file with anything but comments, blank lines and one `repositoryID: <a UUID>`. Then it pushes that file, byte for byte, under the tag `artifacthub.io` alone, in the shape Artifact Hub documents for `oras push` (an empty configuration of the type `application/vnd.cncf.artifacthub.config.v1+yaml`, the file as the one layer of the type `application/vnd.cncf.artifacthub.repository-metadata.layer.v1.yaml`), and reads the manifest and the layer back. No job needs it, so a failure stops nothing of the image (`build/ci/container-image-artifacthub.sh`) |
| `container-image-sbom` | linix, in the Linux CI image | For each platform, Syft reads that platform's pushed image from the registry by its digest, with the job's own registry credentials, and `build/asset-sbom.py image` adds what the release's `.deb` SBOM of that architecture names inside the binary and makes the image, by its digest and its platform (`--arch`), the document's subject; it refuses a `.deb` SBOM that does not describe the `.deb`, or that is another architecture's, and an image in which Syft finds no `hmailserver` package of that architecture (`build/ci/container-image-sbom.sh`) |
| `container-image-sign` | linix, in cosign's image (`HM_COSIGN_IMAGE`) | Signs the index and each platform's image, each by its own digest, with Sigstore, keylessly, as the same pipeline identity the release's files carry, and verifies each signature with that identity. Every tag names the index, so its one signature covers them; a client that pulls a tag then fetches its platform's image by that image's digest, which carries its own. The signatures are stored beside what they sign in cosign's tag form, `sha256-<digest>.sig`, with Rekor v1 as the log. It then attests each platform's SBOM to that platform's image (`cosign attest --type cyclonedx`, stored as `sha256-<digest>.att`) and verifies each attestation with the same identity (`build/ci/container-image-sign.sh`) |
| `container_scanning`, `container_scanning_arm64` | linix, in Trivy's image (`aquasec/trivy:0.74.0`, `dependency-scan`'s, by digest) | Trivy over each platform's pushed image by its digest, with that day's vulnerability database from `mirror.gcr.io` or `public.ecr.aws` (`build/ci/container-scan.sh`): the first scans `HM_IMAGE_AMD64`, from `container-image`'s dotenv report, and the second, which extends it, `HM_IMAGE_ARM64`, each chosen by the job's `HM_SCAN_ARCH`; the script refuses any image but the project's by a full digest. Trivy reads the image's files from the registry without credentials and runs nothing in it, so the platform is no matter to the runner. Each is `allow_failure`, as GitLab's template, which the jobs ran until 10 October 2026, made them: a finding is a warning. The report, in GitLab's container-scanning format and Trivy's own, is the job's artifact, for members with the Developer role or above, because the vulnerability report is GitLab Ultimate |
| `container-image-start`, `container-image-start-arm64` | linix, in the Linux CI image | Each starts a platform's pushed image as a service beside PostgreSQL, on a network of the job's own, through its entrypoint, and asks it for SMTP on 25 (with STARTTLS) and for the REST API over TLS with the release's version: the first the index by its digest, which the runner resolves to linux/amd64, and the second the linux/arm64 image by its own digest, as a service of the platform linux/arm64 (`services:docker:platform`), which the runner runs under qemu-user (`build/ci/container-image-start-test.sh`, which also runs by hand against any image with `--docker`, and `HM_START_PLATFORM` for another platform) |

In 6.3.6's tag pipeline 2914690858 server2 ran `container-image-amd64`,
`container-image-arm64`, `container-image-sbom` and `container-image-start-arm64`,
and linix `container-image-start` ([server2](#server2)).

**Why after `release-publish`.** `:latest` is what the Compose file and the chart
pull. A release the installer smoke test stopped before `release-publish` must
not be what `:latest` names, so the image waits for the release to exist, and is
built from the release's published `.deb` files.

**Why two build jobs and regctl.** kaniko builds one platform a run and cannot
join two images under one index. Each platform has its own kaniko job, and
`container-image` joins and pushes them with regctl, which the channels' registry
mirror already takes from Docker Hub (`HM_REGCTL_IMAGE`,
`.gitlab/ci/channels.yml`): the same release, 0.11.6, in its `-alpine` variant,
whose busybox is the shell a job needs, pinned by digest, and
`build/check-release-chain.py` holds the two to one release. crane
(`gcr.io/go-containerregistry/crane`) could join them as well, but its `index
append` joins images already in a registry, so each platform's image would be
sent before the index naming them could be made and checked; regctl imports
each tarball into an OCI layout of the job's own and makes the index there, so
everything is checked before anything is sent, and it is the tool the project
already takes. `manifest-tool` was not taken because its releases are
GitHub's. The regctl image runs as its own user, so `container-image` cannot take
`.no-github`, which writes `/etc/hosts`; it reaches `CI_REGISTRY` and nothing
else.

The Helm chart is a file of the release, made by `helm-chart-package`
(`.gitlab/ci/helm-chart.yml`) and served from the Pages site's `helm/`, which
`build/ci/helm-repo.py` writes from the feed ([ContainerImage.md](ContainerImage.md)).

**Checked before the push.** Each platform's build job checks its image before
`container-image` starts, and `container-image` pushes the very tarballs those
jobs checked, held to their SHA-256, with each manifest naming the configuration
its job read. So nothing is pushed until both platforms' images have passed, and
no tag is written until both images and the index are in the registry. What
fails after the push - reading the tags back, or `container-image-start` or
`container-image-start-arm64` - names the tags, which are then deleted by hand
(ContainerImage.md, "When a job fails"). The same checks passed on the same
Dockerfile and `.deb` files in `linux-image` and `linux-image-arm64` earlier in
the pipeline; only the Ubuntu packages the Dockerfile installs can differ
between the builds.

**To verify an image**, with cosign 2.4 or later - the tag, which names the
index, and each platform's image by its digest, which `docker manifest inspect`
of the tag lists:

```
cosign verify \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  registry.gitlab.com/hmailserver/hmailserver:<version>
cosign verify \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  registry.gitlab.com/hmailserver/hmailserver@sha256:<the platform's digest>
```

The identity and the image are the group's because the image is pushed only
since the project's move (item 3 below). `container-image-sign` keeps
`container-image-signed/verification.txt`, with the commands for that release.
cosign checks the identity, a path; the project's id, `86730559`, which the
certificate carries too and a server requires, is read from the signatures'
certificates as ContainerImage.md, "Checking an image", shows, and
`container-image-sign` itself holds the certificate of each signature it made
to it.

**What the owner sets on the registry.** Nothing in the pipeline can. As the
project's API answered on 6 October 2026, and on 8 October for the ninth
pattern:

1. **The cleanup policy** is switched off. Its tags to keep
   (`name_regex_keep`) are nine patterns: `^v?[0-9]+\.[0-9]+\.[0-9]+.*$`,
   `^latest$`, `^stable$`, `^[0-9]+\.[0-9]+$` (the `6.3` line),
   `^sha256-[0-9a-f]{64}\.(sig|att|sbom)$` (cosign's signatures and
   attestations), `^artifacthub\.io$` (the tag Artifact Hub reads the image
   repository's metadata from, which `container-image-artifacthub` pushes again
   with each release), `^[0-9]{2}\.[0-9]{2}-[0-9a-f]{12}$` (the Linux CI
   image's tags in `ci-linux`, `<Ubuntu release>-<hash>`, which `.linix` names
   with the digest), `^[0-9]{8}-(amd64|arm64)$` (the Tumbleweed mirror's in
   `mirror/opensuse-tumbleweed`, `<YYYYMMDD>-<arch>`, whose images the RPM
   jobs pin by digest) and, ninth and last as the API lists them,
   `^[0-9]{2}\.[0-9]{2}-[0-9a-f]{12}-[0-9]{8}$` (the dated tags in `ci-linux`,
   `<that tag>-<YYYYMMDD>`, under which `build/ci/build-linux-image.sh --push`
   keeps the digest the tag named before a rebuild moved it,
   [The image](#the-image); a second rebuild's `-2` is outside the pattern),
   so that, switched on, it never deletes a
   release's tags, their signatures, that tag, or an image a commit's CI files
   name; it would remove every other tag older than 90 days and beyond the
   newest ten of its repository, every week. The API gave only the first two
   on 27 and on 28 September 2026, though this item said all five then
   documented were set; the maintainers added the other three through the API
   on 28 September, before the first image, the sixth on 6 October 2026, the
   day the image's repository was added to Artifact Hub, two more the same
   day, for the CI image and the Tumbleweed mirror, and the dated tags' on 8
   October, the day the 6 October digest was kept under the first of them. The
   policy covers every repository of the project's registry: on 6 October 2026
   the image's, `mirror/opensuse-tumbleweed` and `ci-linux`.
2. **Protected container tags**: one rule, for
   `^(latest|stable|[0-9]+\.[0-9]+(\.[0-9]+)?)$`, push Maintainer, delete Owner,
   so that nobody but a maintainer can push the release's tags and nobody but
   the owner delete them. A pre-release's tag (`6.3.5-rc1`) and the signatures
   are outside it. Immutable tags are GitLab Ultimate. `artifacthub.io` is
   outside it too, and it needs the rule more than those: Artifact Hub reads
   from the file under that tag both the image listing's Verified publisher ID
   and the `owners` an ownership claim of the listing is checked against, so
   whoever can write the tag - anybody with the Developer role, or a job of a
   pipeline one of them starts - could put their own address there and claim
   the listing. `container-image-artifacthub.sh` refuses an `owners` key only
   in the tree's file, not in a push made some other way. So a second rule,
   `^artifacthub\.io$`, push Maintainer, delete Owner, protects it: the
   maintainers added it through the API on 2 October 2026, before the image's
   repository was added to Artifact Hub on 6 October, and the API answers with
   it. `container-image-artifacthub` runs with the role of whoever started it,
   or pushed the tag, a maintainer in both of its pipelines.
3. **The switch, `HM_PUBLISH_IMAGE`, stayed unset until the project had
   moved.** The maintainers decided on 27 September 2026 to move the project
   into the group `hmailserver` before the first image is pushed, so that the
   first image's address, `registry.gitlab.com/hmailserver/hmailserver`, is
   the one it keeps. The project moved on 29 September 2026. A container
   repository's path always matches its project's (GitLab's documentation,
   *Container registry*, "Move or rename container registry repositories"), so
   an image pushed from the first path would have been
   `registry.gitlab.com/progressiverobot/hmailserver` and would have changed
   its address with the move, under everyone who pulled or pinned it. The
   signatures were no reason to wait: 6.3.4's `SigstoreVerifier` already trusts
   the group's path, with the project's id `86730559`
   ([UpdateFeed.md](UpdateFeed.md)).

   Until 28 September 2026 this item gave another reason: that GitLab.com
   cannot transfer a project whose container registry holds an image. GitLab's
   documentation does not say that. *Transfer a project* (Manage projects)
   says that for a project whose container registry is enabled, GitLab.com
   transfers the project only within the same top-level group, that a project
   with more than 1,000 container repositories cannot be transferred, and that
   a project must hold no container image on GitLab Self-Managed. The
   *Container registry* page limits renaming a project with populated
   container repositories to projects with up to 1,000 of them.
   This project's registry was enabled and held no repository (the project's
   API, 28 September 2026). The move from the personal namespace
   `Progressiverobot` to the group `hmailserver` changed the top-level
   namespace, so by the first of those rules the registry might have had to be
   switched off for the transfer (Settings, General, "Visibility, project
   features, permissions") and on again after it. The transfer was made on 29
   September 2026; after it the registry was enabled and held no repository,
   and `HM_PUBLISH_IMAGE` was not set (the project's API, that day).

   The project moved only after a release signed by
   this pipeline from its first path, 6.3.5, had shown the project's id
   ([UpdateFeed.md](UpdateFeed.md)), so that release was made with the switch
   unset: 6.3.5's own CI files made its image jobs only if the switch was set
   when its pipeline was made, so its pipeline has none (pipeline 2892097590)
   and the registry stayed empty. Since the move the maintainers can add
   `HM_PUBLISH_IMAGE` with the value `1` as a project CI/CD variable (Settings,
   CI/CD, Variables; a `v*` tag's pipeline is for a protected ref, so it may be
   marked Protected), and start `container-image-deb` in the pipeline of the
   release whose image is to be pushed - one tagged before the variable was set
   included, from 6.3.6 on, since the jobs are in every such `v*` tag's pipeline
   and read the switch as they run. They have: the variable is `1`, protected,
   and 6.3.6's tag pipeline 2914690858 pushed the first image on 6 October 2026.
   It is a project
   variable because the project refuses variables given to a pipeline or a
   schedule, so nobody turns the jobs on for one run. Nothing committed sets
   it: `build/check-release-chain.py` fails when a CI file does, when a rule
   of an image job reads it, and when `container-image-deb.sh`,
   `linux-image.sh` or `container-image-push.sh` does not refuse with a
   non-zero exit while it is not `1`.
   The Compose file and the chart name the group's path;
   `container-image` pushes only at the group's path, and fails at any other,
   the first path included, which is another project's since the move.

A stable release of an older line - a 6.2 release after 6.3.0 - would take
`:latest` from the newer one. The project releases one line at a time, and the
job does not look.

### What waits for the owner

Each is a one-time step, but for the third's credential, which is made for
each release's tag. The first two are done. Until the third is done for a
release's tag, the job that needs it stops with a sentence naming it, and
nothing is signed. The fourth is
met only at `release-publish`'s first upload, after both signatures are made,
and GitLab's refusal is what the job reports. The fifth is a setting nothing in
the pipeline can check, and the one every server's trust in a release rests on.

1. **The cosign image's digest - done, 26 September 2026.** `HM_COSIGN_IMAGE`
   is `gcr.io/projectsigstore/cosign:v2.6.5-dev@sha256:76029a0246bcfde96caa72469a62a1094f6e0978dc3f537f5cafd4c5f6c6a417`:
   cosign 2.6.5, the last release of the 2 line `release-sign.sh` was written
   for (2.4 or later has `--new-bundle-format`, and 2.6 has
   `--use-signing-config`, which the script turns off), in its `-dev` image,
   which has busybox where the plain image has no shell at all. The digest is
   the tag's multi-platform index, read on linix with `docker pull` and
   `docker inspect --format '{{index .RepoDigests 0}}'`, and the same digest
   `gcr.io`'s tag list gives for `v2.6.5-dev`. The image has no `/bin/sh`: its
   shell is `/busybox/sh`, which is on the image's `PATH` as `sh`, and that is
   the name GitLab's runner starts a job's script with. In that image on linix,
   `sh -c 'cosign version'` answered `v2.6.5` (commit 3e82f50a), `sign-blob`
   offered both flags the script tests for, busybox had every tool the script
   uses (`cmp`, `grep`, `sed`, `tr`, `wc`, `find`), and `release-sign.sh`
   parsed and ran up to its first refusal, the missing `release-assets/`. It
   signed 6.3.5's files in `release-sign` on 29 September 2026. A new version is a
   new tag and digest here, never an image from `ghcr.io`, and
   `build/check-release-chain.py` fails on a tag without its digest.
2. **Inno Setup 6 and the Artifact Signing Client Tools on bench150**, as
   administrator ([bench150](#bench150)). Done on 26 September 2026.
   `windows-packages-x64` built the unsigned x64 installer with that Inno
   Setup in pipeline 2887115157 (27 September 2026); `windows-installer` and
   `release-authenticode`, which run only for a `v*` tag, first ran in 6.3.5's
   tag pipeline on 29 September 2026 (130 s and 44 s; the signer
   `CN=Progressive Robot Ltd`, with Microsoft's RFC 3161 timestamp).
3. **The Azure side of Authenticode**: a federated credential for each
   release's tag alone, made before the tag is pushed and removed after, and
   the five CI/CD variables, once (RELEASE.md, "Authenticode", has what to
   create). Not a flexible credential matching every `v*` tag: any Maintainer
   can push one, and release tags are the owner's alone.
4. **The package registry**: on for the project (Settings, General,
   Visibility, Packages). `release-publish` uploads and creates the release
   with `CI_JOB_TOKEN`, which acts with the permissions of whoever started the
   job, so it is started by an Owner: since 26 September 2026 the project's
   package protection rules let only an Owner push to the generic packages
   `hmailserver` and `build-inputs` (Settings, Packages and registries).
   Uploading a file of a name already there is still allowed, so a re-run of
   the chain works. Deleting a package still takes only a Maintainer, because
   GitLab.com has not switched on the rules' delete level.
5. **The protected tag rule, checked.** Settings, Repository, Protected tags:
   `v*`, *Allowed to create* Maintainers, and nobody else. The whole Sigstore
   trust rests on it: a server built after 6.3.3 accepts a bundle whose
   certificate names `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`
   with the project's id, the group's path the project has been at since 29 September 2026, or
   `https://gitlab.com/Progressiverobot/hmailserver//...`, the path it began at
   (`SigstoreVerifier.cpp`), and Fulcio
   issues that certificate to any pipeline of such a tag, running whatever
   `.gitlab-ci.yml` the tag's commit has. Anyone who can push a `v` tag could
   therefore sign a release the servers accept with a CI file of their own,
   without the release jobs. The pipeline cannot enforce this: without the rule
   the release jobs simply do not run, and a hostile file does not need them.
   Check it again whenever the project's members or settings change, and the
   group's too: since the move on 29 September 2026 the group's members and
   Owners can reach the project.

The feed needs nothing here. `pages` and `package-repos` run in the tag's own
pipeline once `release-publish` has passed, with no variable and no token but
the job's own, so the project's *Minimum role to use pipeline variables* stays
at GitLab's default for a new project, *No one allowed*.

### What 6.3.5's run showed

These were written from the documentation before anything had run. 6.3.5's tag
pipeline (2892097590, 29 September 2026) settled the first eight:

- **Shown:** a job which `needs` a manual job waits for it rather than being
  skipped: `update-feed`, made when the pipeline was, started only when
  `release-publish` had passed, four hours later (GitLab documents a blocking
  manual job, `allow_failure: false`, as holding the jobs after it, which is why
  both buttons are blocking, and why `pages` can wait for `release-publish` in
  a tag's pipeline);
- **Shown:** GitLab's Fulcio certificate carries the repository id: 6.3.5's
  records `86730559` beside the identity
  `https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v6.3.5`
  and the issuer `https://gitlab.com`, as its notes say;
- **Shown:** the gcr.io `-dev` tag chosen exists and cosign reads
  `SIGSTORE_ID_TOKEN` from the environment in it: `release-sign` signed every
  file;
- **Shown:** the Artifact Signing plugin's credential chain includes the
  workload identity credential (`AZURE_FEDERATED_TOKEN_FILE`):
  `release-authenticode` signed the installer;
- **Refuted:** that `update-feed` could trigger a pipeline in its own project.
  GitLab dropped it at once with `invalid_bridge_trigger` ("downstream pipeline
  trigger definition is invalid"): `Ci::CreateDownstreamPipelineService` refuses
  a `trigger: project` naming the project the job runs in, whatever the
  pipeline it would make. The feed was deployed that day by a pipeline on
  `master` started through the API, and from 6.3.6 the tag's own `pages` and
  `package-repos` deploy it after `release-publish` ([The update feed](#the-update-feed));
- **Not relied on:** that the generic package upload answers with
  `file_sha256`; the job reads the registry back instead, and did;
- **Shown:** the job token may list the project's packages and their files:
  `release` did before anything was signed, and `release-publish` before and
  after its upload;
- **Shown:** fetching `master` in the job's shallow checkout brings its history
  down to the tagged commit: `release` took `v6.3.5`, an annotated tag on
  `master` signed by the owner's key, on GitLab's runners.

Not yet shown, because 6.3.5 had no image and no AArch64 package:

- that GitLab's registry takes `container-image`'s push with the job's own
  registry credentials: a Docker manifest list naming the two platforms' images.
  The push itself - both platforms' images by digest, the index under `6.3.5`,
  `6.3` and `latest` and nothing else, the read-back, the dotenv report, and ten
  refusals before anything was sent - was run on linix on 29 September 2026, in
  the job's regctl image, from both platforms' release builds of 6.3.5 (its
  published amd64 `.deb`, and batch41's arm64 one, since the 6.3.5 release carries
  none), against a registry standing in for `registry.gitlab.com` (distribution's
  `registry:3`, over TLS with a throwaway certificate, on a docker network of its
  own); and cosign, with a throwaway key and no log, signed the index and both
  images there and attested each image's SBOM (ContainerImage.md, "What was run
  by hand");
- that `container-image-start-arm64`'s service of the platform linux/arm64
  (`services:docker:platform`, which GitLab's CI lint accepts) starts the arm64
  image under qemu-user on linix's runner, as the arm64 package jobs' containers
  start there. The same image, started by hand on linix with the same command,
  passed the start test;
- that `container_scanning_arm64`'s `before_script` hands the analyzer the arm64
  image. The pinned analyzer's version 8 scanned both platforms' images by digest
  on linix on 29 September 2026;
- that the dotenv report reaches `container_scanning`, which names the job in
  `dependencies` where the template leaves it empty. The analyzer, at the digest
  pinned here, scanned an image named by its digest (Ubuntu 24.04's, the
  Dockerfile's base) on linix on 26 September 2026 and wrote its report;
- that cosign stores the image's signature in GitLab's registry as
  `sha256-<digest>.sig` with the job's credentials, and that `cosign verify`
  finds it there.

Shown since in `batch42`'s pipelines 2896082335 and 2897966924 (30 September
2026), and in a release's since 6.3.6's tag pipeline 2914690858, whose
`release-verify` held each of the release's 27 SBOMs to the file it describes:
that the AArch64 package builds' record of what they were built against is what
`build/asset-sbom.py` reads - `dpkg-query -W` in Ubuntu 26.04's container, with
the cross toolchain's
`libc6-dev-arm64-cross` and `libstdc++-15-dev-arm64-cross` beside the `:arm64`
-dev packages, and `rpm --root` over the aarch64 sysroot dnf installed in Fedora
44's. In both, `deb-ubuntu26.04-arm64-sbom` and `rpm-fedora44-aarch64-sbom` made
the generic AArch64 packages' SBOMs from those records, 8 components each, and
in 2897966924 `release-packages` made every per-distribution package's, and the
release's rehearsal held every SBOM to the file it describes.

Dependency updates
------------------

Renovate proposes the project's dependency updates as merge requests on GitLab,
the work Dependabot did on GitHub. Dependabot's updater comes from GitHub's own
registry, and this project takes nothing from GitHub. Renovate is open source, its
image is on Docker Hub (`renovate/renovate`, pinned by digest in the CI file), and
it supports GitLab itself. `renovate.json` at the repository root is its
configuration. The `renovate` job runs it on linix, in a scheduled pipeline and in
nothing else.

**It has run on GitLab since 27 September 2026.** The job needs a token, a CI
variable and a schedule that only the owner can create ([The owner's one-time
steps](#the-owners-one-time-steps)); without them GitLab creates no `renovate`
job. With `RENOVATE_TOKEN` set and the schedule `renovate` made on 27 September
(4461186, `30 2 * * *` in London time, on `master`), its first pipeline,
2886410224, passed that day. What it would propose was first read
from the dry runs of 25 September 2026 ([Reading a dry run](#reading-a-dry-run)).

### What it covers

Every datasource is off GitHub. `renovate.json` also refuses `github.com`,
`githubusercontent.com` and `ghcr.io` outright (`hostRules`), and reads no release
notes (`fetchChangeLogs` off), because release notes come from GitHub.

Fedora's registry is asked one request at a time (a `hostRules` entry for
`registry.fedoraproject.org`, which covers the CDN it redirects every blob to).
Three daily runs in a row each lost one Fedora lookup to a network error, a
different request each time: the tag list for `.gitlab-ci.yml`'s `fedora:44` on
28 and 29 September 2026 ("no-result"), and on 30 September the configuration
blob of `packages-rpm.yml`'s `fedora:43` x86-64 pin, twice `ERR_STREAM_DESTROYED`,
which the dashboard showed as "Could not determine new digest". The pins are
right: for a pin of one architecture's manifest Renovate reads the architecture
from that blob and proposes the same architecture's manifest of the tag's new
index, and a lookup dry run that evening resolved all seven Fedora pins. The
limit is a mitigation, not a proven cure; the job's debug log
(`renovate-log.ndjson`) names the request that failed if it comes back.

| What | Where it is named | Renovate's manager | Where new versions are read (datasource) | How it arrives |
|---|---|---|---|---|
| NuGet, the SDK-style projects | the Control Panel, the tools, their tests, `LinuxRegressionTests` | `nuget` | `nuget`: api.nuget.org | "NuGet packages", weekly, with the lock files |
| The .NET SDK | `global.json` | `nuget` | `dotnet-version`: Microsoft's release metadata | never: a rule turns it off (below) |
| NuGet, the `packages.config` projects | RegressionTests, StressTest, VMTestRunner.Console | a regex manager | `nuget`: api.nuget.org | waits on the dashboard |
| Container images | `build/ci/linux.Dockerfile`, `.gitlab-ci.yml` and the files it includes, the packaging Dockerfile and compose file, `test/Docker/Dockerfile`, the lab's compose file | `dockerfile`, `gitlabci`, `docker-compose` | `docker`: Docker Hub, mcr.microsoft.com and each distribution's and GitLab's own registry | "container images", weekly, every image pinned by digest; a new major version, where one is allowed, in a merge request of its own |
| Images named in a variable | `HM_COSIGN_IMAGE` (`.gitlab-ci.yml`), `HM_REGCTL_IMAGE` (`.gitlab/ci/channels.yml`), and the default of `HM_START_POSTGRES_IMAGE` in `build/ci/container-image-start-test.sh` - a job's `image: $VARIABLE` the `gitlabci` manager cannot read | a regex manager (`HM_<NAME>_IMAGE: "<image>:<tag>@sha256:<digest>"`, or `:-` in a script) | `docker`: gcr.io, Docker Hub | with the container images: `container-image.yml`'s `regctl:<version>-alpine`, which `build/check-release-chain.py` holds to `HM_REGCTL_IMAGE`'s release, moves in the same merge request; cosign stays on 2 (below) |
| Python | the `.in` files of `build/` and `build/ci/` (Semgrep's packages, the build scripts', the command-line package's build tools) and the command-line package's `pyproject.toml`; never the locked `.txt` files uv makes from the `.in` files, which are in `ignorePaths` | `pip_requirements` (the `.in` files only), `pep621` | `pypi`: pypi.org | "Python packages", weekly; the locked files are remade on the branch (below) |
| npm | nothing in the tree today | `npm` | `npm`: registry.npmjs.org | one merge request, weekly, once a `package.json` exists |
| OpenSSL | `hMailServer.vcxproj`, `hMailServer64.iss`, `post-build.bat` | a regex manager | `custom.openssl`: openssl-library.org/source/, the page only (its archive links point at GitHub and are never followed) | a patch release on any day, in "OpenSSL patch release"; otherwise "native libraries", weekly; same major version |
| Boost | the server's and the minidump tool's `.vcxproj`, `fuzz/build-fuzz.ps1` | a regex manager | `custom.boost`: archives.boost.io/release/ | "native libraries" |
| The PostgreSQL client, libpq | `hMailServer.vcxproj`, `hMailServer64.iss`, `post-build.bat` | a regex manager | `custom.postgresql`: ftp.postgresql.org/pub/source/ | "native libraries" |
| zlib, vendored | `Server/zlib/zlib.h` | a regex manager | `custom.zlib`: zlib.net/fossils/ | waits on the dashboard |
| QuickJS, vendored | `Server/quickjs/VERSION` | a regex manager | `custom.quickjs`: bellard.org/quickjs/ | waits on the dashboard |
| The MariaDB client, a build input | `post-build.bat`, `TestEnvironments.cs`, `.gitignore`, `CONTRIBUTING.md`, `third-party-binaries.json` | a regex manager | `custom.mariadb-connector-c`: downloads.mariadb.org's API | waits on the dashboard |
| 7-Zip's `7za.exe`, a build input | `third-party-binaries.json` | a regex manager | `custom.7-zip`: 7-zip.org/download.html, the page only | waits on the dashboard |

Not covered, each for a reason: the MSVC runtime (gathered from the build
machine's Visual Studio), the ADO type library (copied from Windows), SQL Server
Compact (retired, with no release to follow), the Linux build's libraries (the
distribution's packages, which move with the image's digest), the old GitHub
workflows, which are no longer in the tree, the project's own image
(`registry.gitlab.com/hmailserver/hmailserver`, which nothing in the tree pulls
but the Compose file and the chart, by `:latest`, and each release pushes from
its own pipeline; and `registry.gitlab.com/progressiverobot/hmailserver`, the
project's path before its move on 29 September 2026, where no image was ever
pushed), and
the Linux CI image, `registry.gitlab.com/hmailserver/hmailserver/ci-linux`, which is built on linix from
`build/ci/linux.Dockerfile` and pushed by hand, under a tag made from the Dockerfile's own hash
([The image](#the-image)), so that it moves only with the Dockerfile. The .NET runtime the installer bundles has no pin
yet: `build/get-dotnet-runtime.ps1` takes the newest 10.0 by channel. A placeholder
rule in `renovate.json` says what to add when the supply-chain row pins it.

### How updates arrive

- **Once a week, one merge request per ecosystem.** The schedule runs the job
  every day, and `renovate.json` lets routine updates through on Mondays (London
  time) only. It allows the whole day, so a gate that holds linix all night does
  not cost a week. An open merge request is not refreshed on other days either
  (`updateNotScheduled` off). A new major version, where one is allowed, comes in
  a second merge request of its own, never beside routine bumps.
- **An OpenSSL patch release on any day.** It comes in a merge request of its
  own, "OpenSSL patch release", on the first daily run after it is published,
  because OpenSSL's patch releases carry its security fixes and the advisory
  alerts below do not reach the custom datasources: OpenSSL 4.0.3, published on
  29 September 2026 with thirteen advisories against the 4.0.2 the Windows
  installers ship, reached the dashboard as a routine update for the next Monday.
- **Security fixes at once.** `osvVulnerabilityAlerts` is on. A dependency whose
  version has a published advisory gets a merge request of its own, labelled
  `security` and marked `[SECURITY]`, on the next daily run, whatever the day. It
  goes to the newest fixed release, in the same major version where there is one.
  Renovate's default, the lowest fixed release, can stop short when a package has
  two advisories. The dashboard lists every advisory found, fixed or not. The advisories come from OSV's own export on Google
  Cloud Storage: `build/ci/osv-offline-db.py` builds Renovate's database from it
  before each run, because Renovate's OSV package would otherwise download it from a
  GitHub release.
- **Nothing merges itself.** There is no automerge. A Renovate merge request is
  reviewed and folded into a batch like any other branch.
- **What stays put.** A rule in `renovate.json` holds each of these, and changing
  that rule is the way to take the upgrade:
  - OpenSSL, Boost, libpq, zlib and the MariaDB client stay on their major
    version, and Microsoft's SDK image stays on .NET 10.
  - cosign (`HM_COSIGN_IMAGE`) stays on 2, the version the release scripts were
    written for: cosign 3 follows Sigstore's signing configuration, which can send
    a signature to Rekor v2 with no `integratedTime`, which no server built after
    6.3.3 reads (`build/ci/release-sign.sh`). Its digest moves.
  - `global.json`'s SDK is not proposed at all. With `rollForward: latestFeature`
    it is the lowest .NET 10 SDK a build accepts, not the one it uses, which is
    already the newest installed; raising it builds nothing differently and
    refuses every machine below it (on 30 September 2026 the laptop had 10.0.400,
    Alpine's `dotnet10-sdk` for `alpine-suite` is 10.0.303, and Renovate proposed
    10.0.401). It is raised by hand when a build needs a newer SDK.
  - A distribution's own image - Debian, Ubuntu, Alpine, AlmaLinux, Fedora,
    UBI, openSUSE - moves by digest only, wherever it is named. Most tags name a
    release a package is built for or installed on, where a new release is a new
    target, added by hand with its jobs, and moving the tag would drop the target
    it names; the CI image's Ubuntu is the one `linux-build` makes the `.deb` and
    `.rpm` in, and the container image's Ubuntu the release its `.deb` was built
    on. The rest run a tool from the release's own packages (`editorconfig`,
    `chart-and-compose`, `update-helper`, `package-repos`, `ppa-upload`,
    `helm-chart-package`), where a new release is a new version of the tool, also
    taken by hand. On 30 September 2026
    the dashboard offered Debian 12 to 13, Ubuntu 24.04 to 26.04, Fedora 43 and
    44 to 46 (Rawhide in Fedora's registry that day) and openSUSE Leap 16.0 to
    16.1 and to 42.3, the 2017 release, which Renovate's versioning ranks above 16.
  - The test lab (`build/lab/docker-compose.yml`) moves by digest only: its
    matrix is chosen, and [TestLab.md](TestLab.md)'s results are for those
    versions.
  - The databases the CI jobs test against (PostgreSQL 18, MariaDB 13) and the
    packaging compose file's PostgreSQL, which users deploy and whose data
    directory a new major version does not open - each deployment's volume is
    moved, as from 16 to 18 in 6.3.6 ([ContainerImage.md](ContainerImage.md),
    Updates) - stay on their major version; so do GitLab's security analyzers, whose major versions
    change their rules and so their findings (`semgrep-sast` is one of the four
    scans the release job reads).
- **The dependency dashboard.** An issue, "Dependency updates (Renovate)", lists
  everything pending. Some updates wait there for a tick, because Renovate cannot
  make the upgrade: the `packages.config` packages, zlib, QuickJS, the MariaDB
  client and 7-Zip.
- **A new release waits three days** before it is proposed (NuGet, npm, PyPI), so
  a release that is pulled soon after publication is never offered. Security fixes
  do not wait.
- **Few pipelines.** A Renovate branch is rebased only when it conflicts. Its
  merge request runs `checkers`, `harnesses` and the change checks on GitLab's
  hosted runner, like any other, on the project's compute minutes - while those
  are spent the pipeline fails before it starts, and the batch the change is
  folded into runs them on the project's runners - and `mr-dependency-check`
  holds one that brings a version with a Critical or High advisory. A
  `renovate/*` branch is not protected, so no job of its reaches the project's
  runners.

### What a merge request still needs from a person

Each merge request carries its own note of these steps.

- **NuGet: restore on Windows before merging.** Renovate rewrites the lock files
  with a restore on Linux. For the projects that target `net10.0-windows`, a Linux
  restore lists packages the Windows Desktop runtime already carries
  (`System.CodeDom`, `System.Diagnostics.EventLog`), which a restore on Windows
  leaves out. On 25 September 2026 a Linux restore of an untouched `master`, with
  the SDK `global.json` names, rewrote the ControlPanel and DBSetup lock files that
  way. On Windows, on the branch, run
  `dotnet restore hmailserver/source/Tools/ControlPanel.sln --force-evaluate` and
  `dotnet restore "hmailserver/source/Tools/hMailServer Tools.sln" --force-evaluate`,
  and commit what they write; without `--force-evaluate` a restore that finds its
  `project.assets.json` current writes nothing (tried on 29 September 2026 with
  Renovate's Control Panel lock file: up to date, left as Renovate wrote it). Otherwise the Windows gate's build rewrites them on bench150 and
  leaves the bench's checkout modified.
- **Python: remake the locked files.** Renovate changes a version in an `.in`
  file only. Run the `uv pip compile` command the matching `.txt` file's header
  gives, in the image its job runs in (the `.in` file says which), which resolves
  every dependency against the new version and hashes every file PyPI has for it.
  A version the others refuse fails there. For Semgrep, run `build/ci/semgrep.sh`
  and remake its baseline if the new version finds more. Renovate once read the
  `.txt` files too: on 28 September 2026 it moved twelve versions there without
  resolving or hashing them, and uv, resolving them against Semgrep 1.178.0, kept
  none of them.
- **A new SDK in `global.json`**, which Renovate no longer proposes: raise it by
  hand only after the SDK is on every Windows build machine, both benches and
  Alpine's `dotnet10-sdk` package, and after the container-images merge request
  that moves the SDK image has landed and the Linux image is rebuilt.
- **`build/ci/linux.Dockerfile` changed**: the Linux jobs' image tag changes (see
  [The image](#the-image)).
- **Native libraries**: build the new version on every Windows build machine with
  `libraries/build-openssl.ps1`, `build-boost.ps1` or `build-pgsql.ps1`, passing
  `-Sha256` the digest published beside the release. Add that digest to the
  script's table, change the version where the documents name it, and run
  `python build/generate-help-pages.py`.
- **The dashboard's items**: a merge request, once ticked, is a notice. Update a
  `packages.config` package in Visual Studio, which also rewrites the `.csproj`'s
  HintPath and Import paths and its assembly versions; Renovate can change only
  `packages.config`. Replace a vendored library's sources, or publish a new
  build-inputs release with its hashes. Then close the merge request.

### The owner's one-time steps

1. **A token.** On gitlab.com, open the avatar menu, then Edit profile, then
   Access tokens (the older menus call it Preferences, then Access tokens). Choose
   Add new token, with the name `renovate`, an expiry date (gitlab.com allows a
   year at most, so set a reminder) and the scope **`api`**. That scope covers the
   API and Git over HTTPS, which Renovate pushes its branches with. Copy the token.
   Renovate's commits and merge requests are authored as the token's user, so a
   personal token puts the owner's name on them. A project access token (project,
   Settings, Access tokens, role Developer, scope `api`) gives Renovate a bot user
   of its own, but gitlab.com offers project access tokens on the Premium and
   Ultimate plans only. The free alternative is a separate GitLab account for the
   bot, made a Developer of the project, with a personal token of its own.
2. **The CI variable.** In the project, open Settings, then CI/CD, then
   Variables, and choose Add variable. Set the type to Variable, the environments
   to all, and the visibility to **Masked** (or Masked and hidden). Turn **Protect
   variable** on and Expand variable reference off. Set the key to
   `RENOVATE_TOKEN` and the value to the token. A protected variable reaches only
   a protected ref, and the job asks for one.
3. **The schedule.** Open Build, then Pipeline schedules, and choose New
   schedule. Set the description to exactly `renovate` (the job's rule compares
   the whole description, so put nothing else in the field), a custom interval
   pattern of `30 2 * * *`, the cron timezone London, and the target branch
   `master`. Add no variable: the project refuses a schedule's variables ([Which
   pipeline runs what](#which-pipeline-runs-what)). Leave it Activated, and save.
   Daily is for the security fixes: the weekly updates wait for Monday's run
   anyway.
4. **Issues** stay enabled on the project (they are by default), for the dashboard.
5. **The first run.** Start it from the schedule's Run button rather than waiting
   for 02:30. The job log names each branch and merge request. The debug log is the
   job's artifact.

To pause Renovate, deactivate the schedule. To replace the token, make a new one
and change the variable's value.

### Reading a dry run

A dry run does everything but write. It clones, reads every file, looks every
version up, computes each branch with its file changes, and says what it would
have done. On 25 September 2026 it ran on linix, with the pinned image, against the
lab Forgejo's copy of the repository
(`--platform=gitea --endpoint=http://192.168.11.154:3000/api/v1 --dry-run=full`),
with a read-only token. There were four dry runs: `master` as mirrored; the
branch's own tree, which carries this CI file and the Linux image's Dockerfile;
that tree with every box on the dashboard ticked; and that tree with a probe, a
`packages.config` and a project naming Newtonsoft.Json 12.0.1 and System.Text.Json
8.0.0, versions with published advisories. The
Forgejo copy is a pull mirror with merge requests switched off, and Renovate
refuses both before it reads a file. So a small proxy in the container answered
the repository's record as the real project's (not a mirror, merge requests on),
refused every request that was not a read, and logged every request. None was
refused. Every GitHub host was made unreachable inside the container, and nothing
failed for it. `renovate.json` went in through `RENOVATE_X_STATIC_REPO_CONFIG_FILE`,
which reads it as the repository's own configuration. The schedule was forced
open, to show Monday's run.

Run with `LOG_FORMAT=json` and `LOG_LEVEL=debug`, as the job's artifact is. These
are the records to read:

- `Dependency extraction complete` counts the files and dependencies each manager
  found. On the branch's tree: 31 files and 197 dependencies (nuget 6 and 19, regex
  20 and 139, dockerfile 3 and 30, gitlabci 1 and 7, docker-compose 1 and 2).
- `packageFiles with updates` lists every dependency, with its `currentValue` and
  the `updates` a lookup found. The rules have not been applied there yet: a major
  version a rule disables is still listed.
- `DRY-RUN: Would commit files to branch <name>` is a branch Renovate would
  create. Its debug twin carries each changed file whole (`rawContents`), which is
  the diff to read.
- `Branch <name> creation is disabled because dependencyDashboardApproval=true` is
  an update that waits on the dashboard.
- `Vulnerability <id> affects <package> <version>` is an advisory that applies,
  and a branch ending in `-vulnerability` is its fix.
- `Repository finished` with `"result": "done"` means the run went to the end.
- A warning about an internal host is the dry run's own proxy. The job does not
  print it.

What the runs proposed:

- **Container images**, weekly: a digest pin for `python:3.13-slim-trixie` (three
  jobs), `node:22-trixie-slim` and the compose file's `postgres:16`. Python moves to
  3.14. New digests for `postgres:16` in `.gitlab-ci.yml`, for `ubuntu:26.04` and
  `mcr.microsoft.com/dotnet/sdk:10.0` in the Linux image's Dockerfile, for the
  packaging image's `ubuntu:24.04`, and for `windows/servercore:ltsc2022`.
- **Container images, major**: `node` 22 to 24 and `postgres` 16 to 18, in the CI
  file and the compose file. The SDK image's 11.0 and the packaging image's Ubuntu
  26.04 were held back by their rules.
- **NuGet and the .NET SDK**: `global.json` from 10.0.303 to 10.0.401, and
  Microsoft.NET.Test.Sdk from 18.10.0 to 18.10.1 in three projects, with their
  lock files.
- **Native libraries**: libpq from 18.3 to 18.6. OpenSSL 4.0.2, Boost 1.92.0 and
  the MariaDB client 3.4.9 were current.
- **Waiting on the dashboard**: zlib from 1.3.1 to 1.3.2, QuickJS from 2025-04-26
  to 2026-06-04, and 7-Zip from 19.00 to 26.03. Fifteen `packages.config`
  packages, among them NLog 6.2.1, OpenTelemetry 1.19.1 and Microsoft.Identity.Client
  4.90.0.
- **Advisories**: none against any dependency in the tree. The probe's three were
  found (GHSA-5crp-9r3c-p9vr, GHSA-hh2w-p6rv-4g7w, GHSA-8g4q-xg66-9fp4), each with
  its `[SECURITY]` branch to a fixed version. That is the check that the database
  built from OSV's export is read.

On 30 September 2026 the rules for the distributions' images, the lab, the
databases, the analyzers, `global.json`'s SDK and OpenSSL's patch releases were
tried with a lookup dry run on the laptop: the pinned image, Renovate's local
platform (`--platform=local --dry-run=lookup`) on a copy of `batch42`'s tree, every
GitHub host resolving to 0.0.0.0 in the container, and the advisory database built
by `osv-offline-db.py`. `renovate-config-validator --strict` passes. The lookups
found 122 updates. With the earlier rules Renovate kept 116 of them; with the new
ones, 79. The 43 held are every major, minor and patch move of a distribution's
image and of the lab, `postgres` 16 to 18, `mariadb` 11 to 13, `semgrep` 6 to 7 and
the SDK image's 11.0. The digest updates stay, among them new digests for the
Fedora 43 and 44, UBI 9 and 10, Leap 16.0, Tumbleweed and Ubuntu 26.04 package
images. The debug log lists the updates before the rules are applied; the updates
kept were counted by applying `renovate.json`'s rules to that list, which gave
Renovate's own count and order ("N flattened updates found") for both files.

### Image pins

Renovate moves a pin to a newer digest; it does not say when the digest a pin
names has gone. GitLab's registry stops serving a digest of its analyzers within
days of the tag moving on - `semgrep-sast`'s of 26 September 2026 by 5 October,
`secret_detection`'s of 5 October by 7 October, `container_scanning`'s of 5
October by 10 October - and a job whose image is gone fails at its pull
(`runner_configuration_error`), whatever it was for, and only when it runs:
`container_scanning` runs in a release tag's pipeline alone, and is allowed to
fail. `build/check-image-pins.py` finds such a pin before a job needs it.

It reads every image named by digest (`name[:tag]@sha256:<digest>`) in
`.gitlab-ci.yml`, the files under `.gitlab/ci/`, every file under `build/`, and
every Dockerfile, `*.Dockerfile`, Containerfile and Compose file of the tree -
an `image:` line, a `FROM`, a shell default such as `${VAR:-name@sha256:...}`, a
string in a script - and not the documents, which record digests as they were.
It asks each pin's registry for the digest as a runner asks it, without
credentials: a `HEAD` of `/v2/<repository>/manifests/<digest>`, with the
anonymous token the registry's challenge names. A 404, a refusal to that token,
or an answer naming another digest is a pin gone, and fails the check (exit 1),
naming the pin and each file and line that names it. A network error, a 429 or a
5xx is asked again, 5 and then 20 seconds later, and after the third time is a
registry that could not be asked (exit 2), which the job retries. It downloads
nothing, and Docker Hub does not count a `HEAD` against its pull limit. On 10
October 2026 it found 59 pins in 118 places in 37 seconds, and one gone,
`container_scanning`'s, which answered 404 ([Security
scanning](#security-scanning)).

The `image-pins` job runs it daily, on linix or server2, in the pipeline of the
schedule described `renovate`, the pipeline about the pins, beside `renovate` and
needing nothing of it: its self-test first, against a registry it serves on the
loopback, then the check, and a pin gone fails that pipeline. It needs the
network, so it is not one of the checkers, which run its self-test alone. `python3 build/check-image-pins.py
--list` prints the pins and where each is named, asking nothing, and RELEASE.md
has the check run before a release tag. `build/check-release-chain.py` fails
when the job is missing, runs in another pipeline, leaves out the self-test or
the check, or is `allow_failure`.

How long each job takes
-----------------------

**Written before any job had run, and not yet replaced row by row.** 6.3.5's
tag pipeline (2892097590, 29 September 2026), whose every gate passed, took
2 h 42 min for `windows-gate`, 2 h 26 min for `linux-suite`, 49 min for
`static-analysis`, 18 min for `linux-sanitize`, 17 min for `linux-build`, 16 min
for `linux-repro`, 13 min for `linux-cross`, 2 min for `windows-dotnet` and
`windows-installer`, and under a minute for each release job; 5 h 38 min from
its first job to its last, the package jobs' queue on linix included. The first
pipeline (2884863496, 26
September 2026, several of its jobs failing part way) took 2 h 29 min for
`windows-gate`, 2 h 7 min for `linux-suite`, 38 min for `static-analysis`,
20 min for `linux-build`, 16 min for `linux-sanitize`, 13 min for
`linux-cross`, 3 min for `windows-dotnet` and under three minutes for every
other job. A time marked *by hand* is the job's
script run on linix on 25 September 2026 in the job's own image, with six CPUs
and the job's service beside it, as the runner would run it; it leaves out the
runner's checkout, the image pulls and the artifact transfers. Every other time
is an estimate reasoned from the gates run by hand. Replace both with the
measured times after the first pipelines.

| Job | Time | Timeout | Where the time comes from |
|---|---|---|---|
| `checkers` | 4 to 6 min | 20 min | Row 139 estimated about six minutes for the checks together, from their run time on a laptop |
| `harnesses` | 2 to 3 min | 20 min | The same estimate |
| `editorconfig` | 11 s by hand | 10 min | 3,938 files, and the package install |
| `chart-and-compose`, `update-helper`, `package-templates`, `roadmap` | 1 to 30 s by hand | 5 to 10 min | Plus the image pull on first use: the .NET SDK image is about 1.3 GB |
| `linux-build` | 12 min by hand | 1 h | The clang build took 695 s with six jobs; the testers, the OpenPGP round trip and the packages take seconds |
| `linux-suite` | 2 to 2.5 h | 4 h | The whole suite took 75 to 90 minutes on GitHub's hosted runner on 16 September 2026, and it has grown since: the job took 2 h 7 min in the first GitLab pipeline, on 26 September 2026, before the 29 fixtures linked that day for the coverage programme, which took 20 minutes more in a run of their own on linix, against the instrumented server. The timeout was three hours until then; it is the runners' maximum now, so that a suite that grows every batch is not cut off |
| `linux-cross` | 8 min by hand | 1 h | 750 translation units compiled for AArch64 with six jobs |
| `linux-sanitize` | seconds by hand, while it skips | 1 h 30 min | Once `HM_SANITIZE` exists, an estimated 15 to 25 minutes: the core under AddressSanitizer, RelWithDebInfo |
| `linux-db-upgrade-pgsql`, `-mysql` | 16 s and 31 s on the first pipeline, which stopped at 6101 | 20 min | 75 upgrade steps from 6029, and a new database made to compare |
| `linux-package-deb`, `-rpm` | 17 s and 2 s by hand | 20 and 10 min | |
| `linux-image` | 18 s by hand; 44 to 50 s on 29 September 2026 beside a pipeline | 30 min | kaniko over Ubuntu 24.04 and the `.deb` |
| `linux-image-arm64` | 162 to 217 s by hand | 30 min | kaniko over Ubuntu 24.04 for arm64 and the arm64 `.deb`, the `RUN` under qemu-user, on linix on 29 September 2026 beside a pipeline, pinned to four CPUs |
| `linux-sbom` | 6 s by hand | 15 min | |
| `linux-repro` | 11 to 16 min by hand | 1 h | One more run of `linux-build.sh` - the build, the testers and the packages - plus a copy of the tree and the comparison, which take seconds: the second build took 760, 795, 959, 673, 686 and 696 s with six jobs in six runs on linix on 26 September 2026 |
| `windows-gate` | 2.5 to 3 h | 4 h | On 25 September 2026 the two halves took 62 minutes (1,339 tests, on bench150) and 70 minutes (2,126 tests, on the laptop), each with its build. One bench runs both, one after the other, with one build. Add any wait for the lock |
| `windows-dotnet` | 10 to 20 min, estimated | 4 h | Two restores (cold for LocalSystem the first time), two Release builds, MIDL, the tests and five publishes; the timeout allows 200 minutes of waiting for the lock |
| `linux-fuzz` | 65 min | 2 h | Three harnesses of 20 minutes, plus about 3 minutes to build and replay (by hand: 3.5 minutes all told with one minute a harness) |
| `linux-coverage` | about 3 h, estimated | 4 h | By hand on linix on 26 September 2026, in the job's image: the whole suite against the instrumented server took 126 minutes, and the 29 fixtures linked that day 20 minutes more in a run of their own; the testers took under a minute and the report seconds. The instrumented build was not timed on its own; `linux-build`'s took 20 minutes in the first pipeline |
| `static-analysis` | 35 to 75 min | 120 min | Its script, run by hand on linix on 25 September 2026 in the job's image with four CPUs where the job has six, beside other work: 51 min 28 s, of which cppcheck took 2 min 22 s and clang-tidy 49 min for 741 files. 44 min in `batch42`'s pipeline 2897966924; 72 min by hand on linix on 2 October 2026 with six, beside other agents' work (load 17 to 21 on 16 CPUs), clang-tidy 67 min of it for 814 files. Two hours since 6.3.6, when the job lost `allow_failure` and a run cut off fails the pipeline |
| `semgrep` | 3 to 5 min | 30 min | The same, with one CPU: 2 min 36 s to 2 min 50 s, the install and the rules' download included |
| `ci-image-packages` | under a minute | 10 min | The copy takes a second; the rest is the job's start |
| `dependency-scan` | 1 to 2 min | 30 min | The same, with one CPU: 15 to 17 s, the vulnerability database's download included |
| `repo-hygiene` | 1 min | 10 min | The same, with one CPU: 14 to 37 s, the apt install included |
| `semgrep-sast` | 5 to 10 min, estimated | 30 min | It passed in 96 s in `master`'s pipeline 2886389460 (27 September 2026), on a shared runner of GitLab's, as it ran until 9 October 2026. Over batch46 on 9 October 2026, in the job's image with linix's limits: 188 s; with server2's: 254 s. By hand on linix on 27 September 2026, over batch41 in the job's image pinned to two CPUs as a shared runner has: 332 s, with the whole of every file read (84 s before `.gitlab/ci/gitlab-security.yml` gave a rule two minutes and lifted the size limit, when it read neither `RestApiServer.cpp` nor `Portal.js`) |
| `secret_detection` | 17 s in a pipeline | 30 min | In `master`'s pipeline 2886389460 (27 September 2026) it failed on twenty-four vetted test fixtures, which `.gitleaksignore` names since `535e9005c`, on a shared runner of GitLab's, as it ran until 9 October 2026. Its scan of the whole history took under a minute on linix |
| `container-image-amd64`, `container-image-arm64` | 44 to 50 s and 162 to 217 s by hand | 30 min | As `linux-image` and `linux-image-arm64`, from the release's `.deb` files; each keeps a tarball of about 85 MB for `container-image` |
| `container-image` | under a minute by hand | 30 min | Both tarballs imported, the index made and checked, and pushed to a registry standing in for GitLab's on linix on 29 September 2026; the push to `registry.gitlab.com` adds the upload of about 170 MB, and the job the download of both tarballs |
| `container-image-sign` | not measured | 15 min | 6.3.6's tag pipeline 2914690858 ran it (job 16947103144); its time was not read |
| `container_scanning`, `container_scanning_arm64` | 18 s each by hand | 30 min | `build/ci/container-scan.sh` as the jobs run it, in Trivy's image on server2, over each of 6.3.6's two images by digest on 10 October 2026, the database's download (about 10 s) included. GitLab's analyzer, which the jobs ran until then, scanned each image of 6.3.5 by digest in 6 s once its image was there |
| `renovate` | 2 to 5 min | 1 h | The dry runs of 25 September 2026 on linix: 5 s for the advisory database, 9 s to install the .NET SDK, 1 to 1.5 min for Renovate. The first run also pulls its image, about 450 MB |
| `image-pins` | under a minute | 15 min | 37 s from the laptop on 10 October 2026, for 59 pins in 118 places; the self-test about a second |

The runners' own `maximum_timeout` is four hours. GitLab lets a job's
`timeout:` exceed the project's default of one hour, but not exceed its
runner's maximum. linix runs up to three jobs at once ([linix](#linix)), but
its longest line is one job after another all the same: the suite waits for
`linux-build`, so about 12 minutes of build and 2 to 2.5 hours of suite, a
little under three hours, with the package jobs, the image, `linux-repro`, the
cross build and the security scans ([What they cost](#what-they-cost)) beside
them and slowing them, since three jobs of six CPUs ask for more than linix's
sixteen cores. bench150's two jobs add up one after the other, the gate lock
holding the second. So a pipeline for a protected ref takes about three hours
from start to finish, and the Windows gate and the Linux lane decide it between
them.

The update feed
---------------

The `pages` job publishes https://updates.progressiverobot.com, the release feed
installed servers read ([UpdateFeed.md](UpdateFeed.md)). In the Python image it
runs `build/generate-update-feed.py` three times: the self-test, the render of
the project's releases into `public/`, and `--serve-check` of what it wrote,
which reads every address a server builds from the tree served as GitLab Pages
serves it. The script reads GitLab's public API and needs no token and no
variable. What it publishes is what the releases say at that moment, whichever
commit ran it.

The tree includes `releases.atom`, an Atom feed of the same releases for feed
readers ([UpdateFeed.md](UpdateFeed.md), "The Atom feed"). `build/check-atom-feed.py`
holds it to RFC 4287 and to `releases.json`. The self-test runs that checker on
the feed made from its canned releases, with the checker's own negative
controls, and `--serve-check` runs it on the `releases.atom` the site would
serve. So `pages` runs it both ways, and `package-repos` runs it again in its
own `--serve-check`. The `checkers` job runs `build/check-atom-feed.py` with no
argument, as it runs every `build/check-*.py`: that checks the feed the
generator makes from the checker's own releases, with no network.

`pages` no longer deploys the site itself. `package-repos`
(`.gitlab/ci/package-repos.yml`) needs it and takes its `public/`. It adds the
signed apt and dnf repositories when the signing key is set, and nothing when it
is not. GitLab Pages then deploys the result when that job passes. The include
turns `pages`' own deployment off with `pages: false`, so the feed is never live
without the repositories between two deployments. `package-repos` takes its
rules from `pages` (`!reference [pages, rules]`), so the two always run
together. What it does, and the key it needs, are in
[PackageManagers.md](PackageManagers.md), "apt and dnf".

It runs for a push to `master`, for a `v*` tag, and for a pipeline schedule on
`master` (the feed's own is described `update feed`) but the four described
`fuzz`, `renovate`, `export` and `security`, and never for a merge request, a `batch*`
branch or any other branch.
A release is created on GitLab after its tag is pushed, so in a `v*` tag's
pipeline `pages` needs `release-publish` and runs only once it has passed; then
`package-repos` deploys the site with the new release in it, from the tag's own
pipeline. GitLab deploys Pages from whichever pipeline's job succeeds, whatever
its ref, once that pipeline's commit is its ref's newest, and a tag's commit
always is (`Gitlab::Pages::DeploymentValidations`). The need is optional,
since `release-publish` is in a tag's pipeline alone: on `master` and in a
schedule's pipeline `pages` runs at once. The schedule brings the feed up to
date after a release made or edited any other way, and so does running a
pipeline on `master` by hand (Build, Pipelines, Run pipeline), along with every
other `master` job - which is how 6.3.5's feed was deployed (pipeline
2892849251), because its tag pipeline's `update-feed` could not start one.

Until 6.3.6 the release ended with `update-feed`, a trigger job meant to start
a pipeline on `master` running `pages` and `package-repos` alone. GitLab refuses
that: `Ci::CreateDownstreamPipelineService` drops a bridge whose `trigger:
project` is the project it runs in and which is not a child pipeline, with
`invalid_bridge_trigger` ("downstream pipeline trigger definition is invalid"),
before it looks at the pipeline it would make, and a path in `trigger: project`
would have broken again when the project moved into the group on 29 September
2026.
`build/check-release-chain.py` now fails on a `trigger: project` in any CI
file, and on a `pages` that does not wait for `release-publish`.

**A schedule's pipeline runs `pages` and `package-repos` alone, and so does a
downstream one.** A scheduled pipeline is a pipeline on `master` to every job in
the file, and so is a downstream pipeline on `master`, whose
`CI_PIPELINE_SOURCE` is `pipeline` - nothing of this project starts one since
6.3.6, but another project's trigger or the trigger API called with a job token
would - so each of the others says it is not for one: `.own-runners` (and so
every job on the project's runners) and `.not-a-fuzz-pipeline` (and so `checkers`,
`harnesses` and `editorconfig`) begin with `- if: $CI_PIPELINE_SOURCE ==
"schedule" || $CI_PIPELINE_SOURCE == "pipeline"` with `when: never`, and the
four path-bound checks, `db-matrix`, the release jobs and GitLab's two scanners
carry the same rule. `build/check-release-chain.py` fails on
a job that does not, apart from the few whose own rules match nothing on such a
pipeline. `pages` itself skips the three schedules described `fuzz`, `renovate`
and `security`, each of which runs its own job alone (`linux-fuzz`, `renovate`,
`secret_detection`), and the checker fails if it stops skipping any of them or
a rule of the file names `HM_PIPELINE` or `RUN_RENOVATE`, the variables that
chose those pipelines until 26 September 2026 and that the project would refuse.
Without them each scheduled run would start the whole Windows gate on bench150
and the Linux suite on linix. A new job added to the file needs the same, or a
schedule starts it too.

The `checkers` job runs the generator's self-test, which reads the compiled feed
addresses out of `UpdateChecker.cpp` and serves a tree the way Pages does, so a
change that breaks one fails in its own pipeline, on a `batch*` branch included,
and not first in `pages` on `master`.

**Making it.** In the project, Build, Pipeline schedules, New schedule. Set the
target branch to `master` and the interval to once a day (`17 6 * * *`, UTC),
and leave it active. The same from a shell with `glab` signed in as a maintainer:

```sh
glab api --method POST projects/86730559/pipeline_schedules \
  -f description="update feed" -f ref=master -f cron="17 6 * * *" -f cron_timezone=UTC
```

The schedule runs as the maintainer who owns it.

**Checking the deployed feed.** `python3 build/generate-update-feed.py --check
--site https://updates.progressiverobot.com` fails when the site is not what
the releases say now, and names each file that differs.

What each job leaves behind
---------------------------

- **`linux-build`** builds in its container's own filesystem (`/tmp/hm-build`),
  which goes when the container does. The runner's build volume on linix keeps
  the checkout and `linux-build/`, the server binary, its hash, the build log
  and the packages (about 23 MB for the two), until the next job's checkout
  cleans it. The artifact expires after three days.
- **`linux-cross`**, **`linux-sanitize`** and **`linux-fuzz`** build in their
  containers' `/tmp` too. `linux-fuzz` leaves its grown corpus in a cache
  volume of the runner's on linix, on purpose, and `fuzz/bin`, `fuzz/build` and
  `fuzz-results/` in the checkout until the next job cleans it; its artifact
  expires after 30 days. The others' artifacts expire after three and seven
  days.
- **`linux-db-upgrade-*`** and **`linux-package-deb`** leave nothing: their
  databases are in the service containers, which the runner removes. The
  package jobs install into their own job containers.
- **`linux-image`** and **`linux-image-arm64`** remove their tarballs before
  they end and keep only the image's configuration, the server's version line
  and the hashes (`linux-image/` and `linux-image-arm64/`, seven days). kaniko's
  layers are in its own container. **`container-image-amd64`** and
  **`container-image-arm64`** keep their tarballs, about 85 MB each, for
  `container-image`, for seven days.
- **`linux-sbom`**'s two documents are its artifact, kept 90 days.
- **`linux-repro`** copies the tree to `/tmp/hm-repro-source` and builds in
  `/tmp/hm-repro-second-build`, both in its container, and removes both before
  it compares. Its artifact, `linux-repro/` (the report, the second build's
  logs, and its binary and packages only when they differ from
  `linux-build`'s), is kept 30 days.
- **`linux-suite`** lays its tree out inside the checkout (`.hmtest-tree`)
  when the way to the checkout is root's alone, because `after_script` may run in
  a new container from the same image, which sees only the checkout.
  `after_script` copies the server's error logs (and every log,
  compressed, when the job did not pass) into `linux-suite/` and removes the
  tree. It does so for a cancelled job too, and for one that ran out of time
  (`RUNNER_SCRIPT_TIMEOUT`). The tree is root's alone, made under umask 0022, as
  a server run as root has it: a command a fixture runs beside the server as root
  runs on as root only over folders no other account could have put a link in
  ([ServiceAccount.md](ServiceAccount.md), "On Linux, a command run as root"). A
  checkout another account owns or every account may write - the docker
  executor checks out under umask 0000 - is no such way, and the tree is then
  `/var/lib/hmtest-tree` in the job's container: the script copies the same logs
  into `linux-suite/` as it ends, on any exit and on SIGTERM, SIGINT or SIGHUP,
  and a job killed outright keeps none of them. The runner removes the PostgreSQL service
  container when the job ends; `docker volume prune` on linix clears any
  anonymous data volume it leaves. `linux-suite/` stays until the next job's checkout cleans it. The
  artifact expires after seven days.
- **linix** keeps the image, each older tag of it until somebody removes it,
  `postgres:18`, `mariadb:13`, `ubuntu:26.04`, `fedora:44`, the kaniko image,
  the runner's helper image and its build and cache volumes.
- **`windows-dotnet`** leaves its clone in `C:\GitLab-Runner\builds` with the
  build output in it (the next job's checkout cleans that), the NuGet packages
  in LocalSystem's profile, which the next run reuses, and its TRX files as the
  artifact for seven days. No MSBuild or compiler server is left running; one a
  cancelled job leaves is stopped before the next job's checkout
  ([bench150](#bench150)).
- **`windows-gate`** leaves the bench as the split gate leaves it: the bench's
  checkout at the gated commit, the service running that build, `C:\HMTest` as
  the suite left it, and the fetched objects in the checkout's `.git`. The
  job's directory under `C:\GitLab-Runner\builds` keeps `gate\`, `gate.zip` and
  `gate-results\`, a few megabytes that the next job replaces.
  `C:\bench\gate.lock` exists only while a gate holds it. The artifact expires
  after seven days.
- **The security scans** write only into the checkout (`security/`,
  `ci-image-packages/`) and their containers' `/tmp`, which go with the next
  checkout and the container. linix keeps the two images they add,
  `python:3.14-slim-trixie` and `aquasec/trivy:0.74.0`, at their digests. Their
  artifacts expire after 30 days (`ci-image-packages` after one).
- **`renovate`** leaves its image on linix (about 2 GB unpacked) and nothing else
  there: the .NET SDK it installs and the advisory database it builds live in the
  job's container. On GitLab it leaves its `renovate/*` branches, their merge
  requests and the dashboard issue, and `renovate-log.ndjson`, its debug log, as an
  artifact that the project's members can read for seven days.

Security scanning
-----------------

GitHub ran three security workflows until the account was suspended on 18
September 2026: CodeQL, dependency review (with Dependabot's alerts), and
OpenSSF Scorecard. Since 25 September 2026 five jobs in the test stage stand
for them. They run on linix, for a protected ref only (`.own-runners`, like the
builds), and **nothing they use is downloaded from GitHub**: not a tool, not a
rule, not a database. On a `batch*` branch they judge the batch's changes from
`master`, not its last push ([Change checks](#change-checks)).
`static-analysis` and `semgrep` fail a pipeline on a new finding that `master`'s
exceptions file does not accept; the other scans are `allow_failure`. Four of
them hold a release ([Before a release](#before-a-release-the-scans-hold-it)).

`static-analysis` and `semgrep` lost `allow_failure` in 6.3.6 (follow-up N02),
once each had passed over `batch42` with that change folded in, run on linix in
its own image on 2 October 2026. Their last pipeline on `batch42`, 2897966924 on
30 September 2026, had passed both; what each found new since was read first:

- `static-analysis`: a cast in QuickJS's `quickjs.h`, which `batch42`'s QuickJS
  update brought in through `ScriptBinding.cpp`, the one server file that
  includes it. It is upstream's code, and `build/ci/cppcheck-suppressions.txt`
  now leaves every cppcheck finding in the vendored engine's directory out, as
  the job already left out its C and clang-tidy's configuration its headers.
- `semgrep`: a `subprocess.run` with an argument list and no shell in
  `build/check-deck-script.py`, marked `# nosemgrep` with its reason, and the
  MD5 of the greylisting key that `build/db-upgrade-greylist.py` recomputes as
  the server does, in the baseline with its reason.

From then a finding new to a baseline fails a `batch*` branch's pipeline, and a
tag's, where `release` then cannot start, unless an exception in force in
`master`'s `.gitlab/security-exceptions.yml` accepts it (below); a scan with a hole fails it too, and
so does a run past the job's timeout, which is two hours for `static-analysis`
for that reason ([How long each job takes](#how-long-each-job-takes)).
`build/check-release-chain.py` fails when either job is `allow_failure` again.

Two more come from GitLab's own templates (`.gitlab/ci/gitlab-security.yml`,
included from `.gitlab-ci.yml`): `semgrep-sast`, GitLab's Semgrep analyzer and
rules over every language of the tree, the server's C and C++ included, and
`secret_detection`, which looks for a key, token or password in the commits a
pipeline brings. They run for `master` and a merge request - on linix or server2,
and in a merge request's pipeline on GitLab's hosted runner; `semgrep-sast` for a
protected `batch*` branch too, whose
`semgrep-sast-gate` reads its report, and for a `v*` tag, whose release job does.
Their images are pinned by digest, each from the project's copy of GitLab's
image ([Change checks](#change-checks)), since GitLab's registry stops serving a
digest its tag has moved on from. Each leaves its JSON report for Developers and
above to download. `SECRET_DETECTION_DISABLED=true`, as a CI/CD variable, turns
`secret_detection` off. Nothing turns `semgrep-sast` off: GitLab's template
skips it when `SAST_DISABLED` is set, and the job's own rules, which replace the
template's, do not read it. A third, `container_scanning`, and its twin
`container_scanning_arm64`, scan the container image a `v*` tag's release
pushes, each platform's by its digest, after the push, with Trivy rather than a
template of GitLab's (below; [The container image](#the-container-image));
`CONTAINER_SCANNING_DISABLED=true`, GitLab's switch, turns both off.

**`container_scanning`'s scanner.** It is Trivy 0.74.0, in Docker Hub's
`aquasec/trivy` by the digest `dependency-scan` runs, so that Renovate moves the
two together, over each platform's pushed image by its digest, with that day's
vulnerability database, downloaded as the job runs from `mirror.gcr.io` or, when
that does not answer, `public.ecr.aws` (`build/ci/container-scan.sh`). Its report
is in GitLab's container-scanning format (Trivy's own `contrib/gitlab.tpl`), with
Trivy's JSON beside it. Until 10 October 2026 the jobs ran GitLab's Container
Scanning template, whose analyzer, `security-products/container-scanning:8`,
could be neither pinned for long nor copied. GitLab rebuilds that image every
day - analyzer 8.6.35, revision `fa5e43f4`, built at 16:23 UTC on 9 October 2026
and at 16:22 on 10 October, the analyzer the same and both databases new - and
stops serving a digest it has replaced within a day or two: the digests of 26
and 28 September answered 404 on 30 September, and the one taken on 5 October
answered 404 on 10 October. A digest taken in a release's commit could be gone
before its tag's pipeline pulled it, and the job, allowed to fail, would then
lose both scans without a word. And the image may not be copied into the
project's registry, as the other two analyzers are: it carries GitLab's
Enterprise Edition code (the analyzer gem's `ee/`, under GitLab's EE licence,
which allows no copy to be published) and a vulnerability database built with
the GitLab Advisory Database (`/home/gitlab/.cache/trivy/ee`, 114 MB, under
GitLab's Security Alert Database Terms, which forbid copying and mirroring it).
On the Free tier the analyzer read Trivy's own database, which this reads too.
Run as the jobs run it - `.no-github`'s `before_script`, then the script, in the
pinned image - on server2 on 10 October 2026, it scanned 6.3.6's two published
images by digest in 18 s each, 19 findings each (15 Medium and 4 Low), each
report naming its platform, and refused a missing or unknown platform, an image
named by a tag, from another registry or by a short digest, a digest the
registry does not have, and a run with no database. `build/check-release-chain.py`
fails when `container_scanning` runs in another image than `dependency-scan`'s
Trivy, by another release or digest, does not run the script, scans another
platform or keeps no report, when `container_scanning_arm64` is anything but its
twin for linux/arm64, when the script no longer takes each platform's image by
its digest, the project's alone, and when a CI file names GitLab's
container-scanning analyzer or includes its template again.

`semgrep-sast` passes whatever it found, and holds nothing to a baseline; what
holds a change or a release on its findings is the job that reads its report
after it, `semgrep-sast-gate` for every change and `release` for a tag. It is
not `allow_failure`, where its template makes it so (after 6.3.6): a scan that
fails, or an image that cannot be pulled, fails the pipeline, so that the jobs
after it read only a report it wrote. **`secret_detection` fails on a finding.** The template cannot:
its analyzer exits 0 whatever it finds (it did so on linix with thirty
findings), and the template marks the job `allow_failure: true`, which would
show a failure as a warning anyway. So the job runs the analyzer, then reads
its report, `gl-secret-detection-report.json`, and fails unless its list of
findings is empty, naming the files; and it is `allow_failure: false`. What was
read and found not to be a secret is named in `.gitleaksignore` at the root, one
gitleaks fingerprint a line (`<commit>:<file>:<rule>:<line>`), which gitleaks
reads from the checkout. A real secret is revoked where it was issued, not
listed.

**The whole history, once.** A scan of every commit rather than the pushed
ones is the pipeline of a schedule on `master` described `security`: there
`secret_detection` runs alone with `SECRET_DETECTION_HISTORIC_SCAN` set by its
own rule. It is a schedule, not a variable set by hand, because the project
refuses variables given to a pipeline or a schedule; and not a manual job,
which would sit in every `master` pipeline and still need a pipeline to start
in. The owner makes it once: *Build*,
*Pipeline schedules*, *New schedule*, description exactly `security`, any
interval (it is not left active), target branch `master`, no variables; then
*Run* it once, and deactivate it. On 26 September 2026 the same scan was run on
linix in the job's image over a full clone of the project - `master` at
40c7d3e0c and its 83 tags, among them the upstream hMailServer 5.x tags whose
history `master` does not contain: 2,214 commits,
720 MB of changes, under a minute, and thirty findings, each read in its commit
and none a secret - PEM and OpenPGP armour lines in the server's key-parser
testers, the example access key id of AWS's own Signature Version 4
examples in `ObjectStoreSignature.cpp`'s tester (described here, not quoted:
quoted, it is a finding in this document and in the help pages made from it), the regression tests' own fixture keys,
and the sample keys under `hmailserver/test/SSL examples` and Boost's asio
examples. They are the thirty lines of `.gitleaksignore`; with the file in the
checkout the scan found nothing, and the job's check passed, and it failed on
the report of the thirty. The schedule's run on GitLab is still to be made. It
scans what the job's checkout reaches, which may be less than the clone did (five
of the thirty are in a commit only the upstream tags reach) but not more while
`master` is the project's one branch.

**Proved by hand first, then in the pipelines.** The three jobs have run in
GitLab's pipelines since the first, 2884863496 on 26 September 2026, which all
three failed ([What the first pipeline found](#what-the-first-pipeline-found));
in `batch42`'s 2896082335 (30 September) all three failed again, on findings new
since their baselines, which `108d3850f` answered the same day - most fixed in
the code, one taken into the static-analysis baseline with its reason, the
dependency scan's PyJWT advisories answered by VEX statements and its finding in
the CI image's OpenSSL by a rebuild of the image - and in 2897966924 all three
passed. Before any pipeline, each job's script was run on linix on 25 September
2026, in a container of the job's own image, with the job's own commands, on a
copy of the tree checked out as GitLab checks it out (CRLF, the scripts LF). The
dependency scan was run with `ghcr.io`, `github.com` and GitHub's content hosts
resolved to 127.0.0.1, and it completed. The numbers below come from those runs.
What they found is in the two baselines:

- `static-analysis`: 1,307 findings. cppcheck made 474 of them, 378 of which
  are `throw;` statements with no exception to rethrow, 376 of them in the
  in-process testers. clang-tidy made 833, led by 164 constructors that leave a
  member uninitialised and 158 multiplications done in `int` and then widened.
  34 more, in Boost's and the C++ library's headers where the static
  analyser's paths end, and 3 in the sources a `build/generate-*.py` writes, are
  counted apart and not held against the tree: a finding in generated code is
  fixed in its generator, and the flagged line moves whenever a document does.
  The baseline was written from that run's raw output by the final
  `security-findings.py`. Read again on Windows from the same output, in
  another checkout, it gave the same fingerprints and nothing new.
- `semgrep`: 85 findings, none in the C#, the webmail's scripts or the Control
  Deck. 44 are in the packages' shell scripts: 39 unquoted expansions in
  `autoban-hook`, all of constants or of arguments it checks to the character
  first, and 5 in `hmailserver-update`, three of them the IFS it changes to
  split a request into lines. 40 are in scripts under `build/`: shell quoting,
  five Python checkers that parse the repository's own XML with the standard
  library, and one `subprocess` call. The last is `hmctl`'s unverified TLS
  context, which it creates only for `--insecure` to a loopback address and
  refuses for any other host.

Negative controls: two lines taken out of a baseline came back as exactly
those findings, new (exit 1); a file clang-tidy could not parse, added to its
output, failed the run as a hole (exit 2); and a missing cppcheck output failed
it too (exit 2).

### Before a release: the scans hold it

`allow_failure` keeps a `dependency-scan` finding from stopping a push's
pipeline, `semgrep-sast` stops a change only through `semgrep-sast-gate`, in
the files the change touches, and a baseline keeps a known `static-analysis` or
`semgrep` finding from it. None of them lets one into a release. A `v*` tag's
`release` job needs four scans of the tagged
commit - `static-analysis`, `semgrep`, `dependency-scan` and GitLab's
`semgrep-sast`, which `.gitlab/ci/gitlab-security.yml` runs for a protected `v*`
tag for this - with their artifacts, and not optionally, so a tag's pipeline
without one of them is not created (GitLab's own check, tried with the CI lint's
dry run for a tag on 26 September 2026). Before anything is collected, let alone
signed, `build/ci/release-chain.py collect` asks `build/ci/security-gate.py`,
which reads the four reports and refuses when:

- a report is missing, or says its scan's status is not `success` - a scan that
  did not finish has said nothing is clean; and when `dependency-scan` did not
  scan the CI image's packages, which the Linux packages are built against;
- `static-analysis` or `semgrep` found something that is not in its baseline
  (the finding's fingerprint, which the gate prints; the reports carry it since
  26 September 2026 as an identifier of type `hmailserver_fingerprint`);
- `semgrep-sast` found something of severity Critical or High, fingerprinted the
  same way from GitLab's report (the rule, the file, the text of the line);
- `dependency-scan` found a Critical or High vulnerability with a fixed version
  that the VEX document does not dismiss - unless it is against a header-only
  package of the CI image (`linux-libc-dev`; [Dependency
  findings](#dependency-findings-the-vex-document-and-what-osv-cannot-answer)),
  which is not affected: the gate says what it spared, and never holds it;

and `.gitlab/security-exceptions.yml` does not accept that finding - `master`'s
copy, which the job reads as it reads `master`'s allow-list, never the tagged
tree's, so an exception that reaches `master` after the tag, through a merge
request of its own, counts when the job is retried. An entry there names the
scan and the finding (a fingerprint; for `dependency-scan` the advisory's id and
the package), what it is, why it is not exploitable (a false positive is one
reason; a fix waiting upstream is none), the GitLab user who read it, the day,
and an expiry at most a year after it. After the expiry the finding holds the
release again until it is read again. The file is a strict subset of YAML (the
job's image has no YAML library), is the owner's in `.gitlab/CODEOWNERS`, and
`build/check-release-chain.py` reads it on every pipeline, so a malformed entry
fails `checkers` long before a release. It held 500 entries on 6 October 2026,
232 of `semgrep-sast`, 264 of `static-analysis` and 4 of `semgrep`, in groups
whose comments say how each was read; the first was the 169 below. The
thresholds are `SECURITY.md`'s ("Vulnerability management policy"); the gate's
self-test, run by `checkers`, holds it to each of them.

What it held first: GitLab's semgrep analyzer, run on linix on 26
September 2026 in the job's own image (90 seconds on two CPUs) over the tree of
the local `v6.3.4` tag as it then was - `master` at 40c7d3e0c, before batch40;
the tag is made again once batch40 lands - found 282 findings, 176 of them High and 106 Medium, none
Critical. 154 of the High ones come from its C and C++ rules ported from
flawfinder - 97 `strlen`, 33 `memcpy`, 11 `MultiByteToWideChar`, 5 `getenv` and
a few more - 21 from the C# rule SCS0001 and one from a Node SSRF rule. The gate
read that report, gave the 176 High findings 176 distinct fingerprints and held
on every one; a trial exception took exactly one of them out. All 176 were
read on 26 September 2026, each by two AI review agents, the second trying to
refute the first, under the owner's delegation: 169 were judged not
exploitable and are accepted in `.gitlab/security-exceptions.yml` until 25
September 2027, each with the check, size or constant that makes it safe; their
`reviewed-by` names the owner, who delegated the reading. The seven judged real
have no entry: each was fixed instead, in the tree 6.3.4 is released from (the
list is in the exceptions file's header), and a fixed line the analyzer still
flags holds the release until it is read again. Over that report, the gate held
on those seven alone; no semgrep-sast scan has been run over the fixed tree yet.

This is the second evaluation, over the whole tree. The first is every
change's ([Change checks](#change-checks)): `semgrep-sast-gate` and
`mr-dependency-check` hold a change on what it adds, and in a batch's pipeline
`static-analysis` and `semgrep` hold it on a finding new to their baselines that
`master`'s exceptions do not accept. The release answers for the rest as well: a
finding already on `master` in a file no change has touched since, an advisory
published after the change that brought its dependency, and `dependency-scan`'s
findings over the whole tree, which hold no change.

The same gate runs by hand where a release is made by hand: the scans' scripts
on linix against the release's commit, then
`python3 build/ci/security-gate.py --artifacts <the directory they wrote to>`,
which exits 1 while anything holds the release.

### Before a push: the secret scan

GitLab's push protection, which refuses a push that carries a secret, is
GitLab Ultimate. What the Free tier allows is built instead, before the push:

- **Hooks the repository ships.** `build/hooks/pre-commit` scans the lines a
  commit adds and `build/hooks/pre-push` the commits a push brings, with
  `build/scan-secrets.py`; `git config core.hooksPath build/hooks` turns them
  on in a clone (CONTRIBUTING.md, "Secrets"). git runs no hook a clone has
  not turned on, so they refuse nothing in a clone that has not.
- **The landing.** `build/land-via-mr.py` scans every commit a batch brings to
  `master` (`build/scan-secrets.py`, over the commits from `master` to the tip)
  before it pushes the branch, and pushes nothing when it finds a secret, or when
  the scan cannot run ([Landing on master](#landing-on-master), step 1).
- **`secret_detection`**, above, fails the pipeline of a merge request, or of a
  push to `master`, on a finding `.gitleaksignore` does not name - after the
  push, and not for a push to another branch without a merge request. A batch's
  own pipeline does not run it, and `master`'s runs it only once the batch has
  landed, so for a batch the landing's scan is the one that reads its commits
  before they reach `master`.
- **`.gitignore`** keeps the usual key and credential files (`*.pem`, `*.key`,
  `*.pfx`, `*.p12`, SSH private keys, `.env`, `.netrc` and the like) out of a
  `git add`, but for the test fixtures' directories, which hold throwaway keys on
  purpose.

`build/scan-secrets.py` runs gitleaks from the image `secret_detection` pins,
the project's copy of GitLab's analyzer - it reads the digest from
`.gitlab/ci/gitlab-security.yml`, so the two cannot differ - with the rules the
job makes (below) and the repository's `.gitleaksignore`: in Docker on the machine, or in Docker
on another machine that sees the repository's git
directory (`HM_SECRET_SCAN_SSH=user@host:/path/.git`, as linix sees the
maintainer's clone at `/mnt/hmailserver`), or, without Docker, a `gitleaks` on
`PATH` with gitleaks' own rules. With none of them it refuses (exit 3). The
scan's container has no network. It prints each finding as gitleaks
fingerprints it and never the secret, and lists any `.gitleaksignore` line the
range adds, which the reader checks. Its first run, on 26 September 2026 over
`master..batch40` (35 commits, seven seconds on linix), found ten findings
`secret_detection` would have failed the next push to `master` on: AWS's example
access key id, quoted in this document, in `.gitleaksignore`'s comment and in
`HelpPagesData.cpp`, which every regeneration of the help pages added again.
The three now describe it instead, and the ten are in `.gitleaksignore`, with
two more the same key at `HelpPagesData.cpp`'s lines 355 and 356 in two folds
made after that run, which a scan of the batch rebased onto `master` found:
twelve lines, naming the commits as they are after the rebase.

#### What the secret scans read

GitLab's rules (0.25.1, `/gitleaks.toml` in the analyzer's image) end in a global
allowlist of paths that gitleaks never reads, and two of its entries left out
every path under a directory named `build`, `bin`, `obj`, `target` or
`packages`: until 10 October 2026 neither `secret_detection` nor
`scan-secrets.py` read `build/` - the CI scripts, the git hooks, the lab, the
fixtures and their builders - or `snap/local/bin/`, 368 files of the tree then.
On 9 October 2026 a made-up GitLab token committed under `build/` passed both
scans, where one at the root of the tree failed both.

Both now run GitLab's rules but for those two entries: the
`(?:target|build|bin|obj)` entry is taken out, and `packages` out of the
`(?:node_modules|bower_components|packages)` one, so that `node_modules` and
`bower_components` stay out as GitLab has them. `build/ci/secret-detection-rules.sh`
derives the rules in the analyzer's image from GitLab's own file, and changes
nothing else of it; it takes nothing from the tree, so that no committed file can
widen what the scans leave out. `secret_detection` runs it before the analyzer
and puts what it writes in place of `/gitleaks.toml`, which the analyzer reads;
it runs as root in its image, and the file is replaced in the job's own
container. GitLab's custom ruleset, which could name another file, is not read
on the Free tier (the analyzer loads `.gitlab/secret-detection-ruleset.toml` only
when `GITLAB_FEATURES` names `sast_custom_rulesets`), and gitleaks' `[extend]`
adds a file's allowlists to its base's and takes none away, so neither could do
it. `scan-secrets.py` runs the same script in the same image before gitleaks:
its own tree's copy of the script, never the scanned range's. The script names
the hash of the GitLab table it was written for (`GITLAB_TABLE`), from GitLab's
`[[allowlists]]` line to the end of the file, and refuses, and both scans with
it, when the image's does not hash to it - a rules version that changes what
GitLab leaves out is read, and the script brought up to date, before either
scan takes it - and when either entry it changes is not on exactly one line of
the table. A change to the script changes what both scans read, so it is one of
the gates' own files (`build/land-via-mr.py`'s `GATE_FILES`): a batch that
changes it lands only with `--gates-read`, a person's word that the change was
read, and the landing's scan prints the lines a range changes in it.

Whose copy of the script runs differs. `secret_detection` runs the pipeline's own
commit's, as it runs every file of its CI, so in a merge request's pipeline the
change's own copy makes the rules that scan it - which is why the script is a
gate file. `scan-secrets.py` hands its container its own tree's copy, never the
scanned range's, and `build/land-via-mr.py` runs `scan-secrets.py` from its own
tree: so a landing run from a checkout of `master`, as a landing is, scans a
batch with `master`'s scan and `master`'s rules, and no code of the batch runs in
it, while one run from the batch's own worktree would have the batch's copies of
both scripts judge the batch. On server2 on 10 October 2026 a commit that put a
made-up token under `build/` and made the rules script leave every path out
passed the job of its own pipeline, as it must, and failed the landing's scan run
from the tree before it.

Read over every branch and tag of the project on GitLab on 10 October 2026
(2,873 commits, `master` at 74aed9ab8) and over `master`'s tree, in the image
`secret_detection` pins, the widened rules found everything GitLab's had found
and five more, the five `.gitleaksignore` names since, none a secret: the
secret keys of the OpenPGP fixtures' alice and bob in `build/fixtures/openpgp`,
whose README says what they are, and three lines that test a text for a private
key's armour header, in `build/ci/installer-smoke.ps1`,
`build/generate-package-repos.py` and `build/deck-script-test.js`. The
analyzer's own whole-history scan named the three lines and not the two keys,
which `.gitattributes` marks binary. What GitLab's table still leaves out stays
out: media and archives by their extension (2,810 files of the tree, its images
and SVGs among them), `.editorconfig`, `.gitattributes` and the like, lock
files, and the vendored and cached directories of other ecosystems, none of
which this tree has.

### The old GitHub jobs and what replaced them

| On GitHub | What it did | On GitLab | What is not replaced |
|---|---|---|---|
| `codeql.yml`, C++ | CodeQL `security-extended` over the MSVC build of the server, on every push and pull request (40 to 54 minutes) | `static-analysis`: cppcheck, and clang-tidy with the Clang static analyser, over every translation unit the Linux build compiles | The Windows-only sources (the COM API, the service host, the ADO and SQL Server Compact backends, the registry) and the Windows arm of every `#ifdef`, which only an MSVC build compiles; CodeQL's interprocedural taint tracking. `build\analyze.ps1` (MSVC `/analyze`) still covers the Windows build by hand |
| `codeql.yml`, C# | CodeQL `security-and-quality` over the Control Panel and the setup and migration tools | `semgrep`: Semgrep's open C#, Python, JavaScript and bash rules. It also reads the Python tools, the shell scripts, the webmail's scripts and the Control Deck's page, which CodeQL did not | CodeQL's type-aware queries and its quality queries |
| `dependency-review.yml` | Failed a pull request that added a dependency with a high or critical advisory, or a package flagged as malware | `dependency-scan`, fed by `ci-image-packages`: Trivy over the lock files and the CI image, OSV over the vendored libraries. It fails on a high or critical advisory that has a fixed version. And since 6.3.6, on every change, `mr-dependency-check`: every version a change adds, against OSV's export, a malicious package (OSV's `MAL-` records) counted as Critical, and every native or vendored library pin it moves, against OSV by upstream tag | GitHub's own malware advisories, where OSV has no `MAL-` record for the package |
| Dependabot | Alerts, and a pull request for each advisory and each new version of an action | The alerts are `dependency-scan`'s | The update pull requests. A bot that opens merge requests (Renovate runs on GitLab) needs a bot account and a token, and that is a decision of its own |
| `scorecard.yml` | OpenSSF Scorecard, published to the OpenSSF API and the README's badge | `repo-hygiene`: Branch-Protection, Signed-Releases, Security-Policy, Pinned-Dependencies, Binary-Artifacts and Token-Permissions, answered from GitLab's public API and the tree. And since 6.3.6 Scorecard itself, weekly, published on the project's own site with its badge ([OpenSSF Scorecard](#openssf-scorecard)) | The OpenSSF API's copy of the score, which takes results from GitHub's action alone, and the checks Scorecard cannot answer on GitLab |
| Secret scanning with push protection | Refused a push that carried a live secret | `secret_detection`, GitLab's free pipeline secret detection, for `master` and a merge request: it finds a secret after the push and fails the pipeline; the `security` schedule scans the whole history | The refusal. Push protection is a GitLab Ultimate feature |

### The jobs, and where each tool comes from

| Job | Image | Tools, and where they come from | Reports | Measured on linix |
|---|---|---|---|---|
| `static-analysis` | the CI image | cppcheck 2.19.0 and clang-tidy 21.1.8, from Ubuntu 26.04's archive, installed as the job starts. clang-tidy is the image's own clang 21 | `gl-sast-report.json` (both tools; each tool's own beside it), `junit.xml` | 51 min 28 s on 4 CPUs |
| `semgrep` | `python:3.14-slim-trixie`, pinned by digest (Docker Hub) | Semgrep 1.178.0 and 65 packages it needs, from PyPI, with every file's SHA-256 pinned (`build/ci/requirements.txt`, `pip --require-hashes`). The rules come from Semgrep's registry at semgrep.dev (`p/csharp`, `p/python`, `p/javascript`, `r/bash`), fetched with no account and no token | `gl-sast-semgrep.json`, `junit.xml` | under 3 min on 1 CPU |
| `ci-image-packages` | the CI image | none | the image's `etc/os-release`, `etc/lsb-release`, dpkg status and the .NET SDK's `deps.json` files, 1.4 MB | seconds |
| `dependency-scan` | `aquasec/trivy:0.74.0`, pinned by digest (Docker Hub) | Trivy 0.74.0; its vulnerability database from `mirror.gcr.io`, with `public.ecr.aws` as the fallback; OSV's API at `api.osv.dev`; Python from Alpine's repository | `gl-dependency-scanning.json` (both scanners; each scanner's own beside it), `junit.xml` | 15 to 17 s, the database download included |
| `repo-hygiene` | `python:3.14-slim-trixie`, pinned by digest | git and ssh-keygen, from Debian; GitLab's API, read anonymously | `junit.xml`, `repo-hygiene.json` | 14 to 37 s, the apt install included |

Trivy downloads its database from `ghcr.io` unless it is told otherwise. Its
default list is `mirror.gcr.io/aquasec/trivy-db:2` and then
`ghcr.io/aquasecurity/trivy-db:2`, and `--db-repository` replaces the whole
list. Trivy moves down its own list only after an error it takes for a
temporary one: with `mirror.gcr.io` unreachable, a list of the mirror and AWS's
copy failed at the mirror and never tried the copy (measured on 25 September
2026). So the script downloads the database itself, naming one repository at a
time: `mirror.gcr.io` first, then `public.ecr.aws/aquasecurity/trivy-db:2`.
It then scans with `--skip-db-update`. Run with `mirror.gcr.io` resolved to
127.0.0.1 as well as GitHub's hosts, the job took the database from AWS's copy
and gave the same answer. A run that can reach neither fails; it never tries
GitHub. Both databases, and OSV, include GitHub's advisory database among their
sources. That is GitHub's data, not a download from GitHub.

### What GitLab's free tier shows

The project is on GitLab's free tier: its namespace's plan read `free` on 25
September 2026.

- **The SAST and dependency-scanning reports** are uploaded with the job as
  reports, and are also in its ordinary artifacts (`security/`). A job uploads
  one file per report type: GitLab refuses a second ("only one file can be sent
  as raw"), which failed `static-analysis` and `dependency-scan` on the first
  pipeline, so each writes one report of all its tools' findings as well as
  one per tool. On the free
  tier they can be downloaded from the job's page and from the pipeline's
  download menu, and that is all. The pipeline's *Security* tab, the
  merge-request security widget, the vulnerability report and the dependency
  list are GitLab Ultimate features. If the project is ever given Ultimate (GitLab
  for Open Source grants it to qualifying projects), the same reports appear
  there with no change to the jobs.
- **The JUnit reports** are what the free tier shows: the pipeline's *Tests* tab
  lists them. So every job writes one as well. The two source scans write one
  test case per tool that fails when anything is new beyond the baseline that no
  exception accepts, one that fails when a file could not be analysed, one
  failing test case per new finding, and a passing one per finding an exception
  accepts. `dependency-scan` writes one test case per lock file, for the CI image
  and for each vendored library, skipped when the answer is unknown; the CI
  image's counts the advisories against `linux-libc-dev`, a header-only package,
  apart from the rest, as not affected (below).
  `repo-hygiene` writes one per check, skipped when the answer is not public.
- **The job log** prints each job's table: counts by tool and rule, every new
  finding, and every hole in the scan.

### The baselines, and triage

`static-analysis` and `semgrep` hold their findings to a committed baseline:
`build/ci/static-analysis-baseline.txt` and `build/ci/semgrep-baseline.txt`. A
finding's fingerprint is a hash of the tool, the rule, the file and the text of
the flagged line with its whitespace collapsed. It is not the line number, so an
edit elsewhere in the file does not make an old finding new. When the same rule
fires on two identical lines of one file, their order counts too.

A baseline is a closed list: the findings that were in the tree before every
change was gated, written when the scans began, on 25 September 2026, and last
added to in 6.3.6. It only shrinks ([SECURITY.md](../../SECURITY.md),
"Declaring a finding not exploitable"). A finding new to it is not added: it is
fixed, or `.gitlab/security-exceptions.yml` accepts it as not exploitable, as
`master` has the file. Each scan reads `master`'s copy, never the commit's own:
`static-analysis` fetches `master` from the checkout's origin with git (which it
installs with its tools, since the CI image has none) and takes the file from
it; `semgrep`, whose image has no git either, reads it from the project's API
without a token
(`https://gitlab.com/api/v4/projects/86730559/repository/files/.gitlab%2Fsecurity-exceptions.yml/raw?ref=master`,
three tries). `build/ci/security-findings.py` reads the file with
`build/ci/security-gate.py`'s own reader, so that what a scan accepts and what
the release's gate accepts cannot differ: an entry naming the job as its scan
and the finding's fingerprint, in force, accepts it. The script compares the
fingerprints and exits with:

- 0 when nothing is beyond the baseline that an exception in force does not
  accept;
- 1 when a new finding is one no exception accepts;
- 2 when the scan has a hole: a tool that did not finish, a file cppcheck or
  clang-tidy could not parse, a file Semgrep could not parse or parsed leaving more
  than ten of its lines unread (Semgrep reports both only as a warning, and its
  bash parser cannot read a `case` written on one line, or an `if` whose condition
  holds a here-document; a line Semgrep names more than once is one line), or a
  Semgrep rule that did not finish with a file - it timed out, ran out of memory
  or of stack, failed while matching, or stopped at too many matches, each also
  only a warning. A hole is never a pass, and no baseline excuses it. And 2 when
  `master`'s exceptions file cannot be read, is not well formed, or has an entry
  reviewed after today: that file then accepts nothing.

So without `allow_failure`, which `static-analysis` and `semgrep` have not had
since 6.3.6, a new finding that no exception accepts fails the pipeline, and an
old one does not. An exception within 30 days of its expiry is named with a
WARNING in the job's log, which still passes; once it lapses, the finding fails
every branch's pipeline again. Besides the tools' output and reports, each job's
artifacts hold `findings.txt` (every finding), `new-findings.txt` (the new ones,
each with its fingerprint), `accepted-findings.txt` (those beyond the baseline an
exception accepts, with its expiry and reviewer) and
`master-security-exceptions.yml` (the copy the run was judged by).

To triage a new finding:

1. Read it: the job log, `new-findings.txt`, or the failing test case in the
   *Tests* tab.
2. If it is a defect, fix it in the change that brought it.
3. If it is not exploitable - a false positive is one such case - accept it in
   `.gitlab/security-exceptions.yml`, with the fingerprint the job printed and
   the reason, in a change of its own that lands on `master` first; then the
   change that brought it passes. Not with a comment beside it: a change that
   adds `// cppcheck-suppress <id>`, `// NOLINT(<check>)` or `# nosemgrep` to a
   file a scan reads is held by `semgrep-sast-gate` ([Change
   checks](#change-checks)), as is one that adds a line to a baseline.
4. If it is real and not to be fixed now, it is not accepted: it holds, and
   SECURITY.md's thresholds say by when it is fixed. A fix that waits upstream is
   no reason to accept one.

A baseline's entries are triaged too: an entry with no reason is an open
finding. Its line is deleted when its finding is fixed, or ended with a tab and
the reason its triage gave; nothing else changes a line by hand. On 6 October
2026 the eight High entries without a reason were given theirs, and 475 of
`static-analysis`'s 691 entries and 62 of `semgrep`'s 125 had none; the rest are
triaged by 31 March 2027. The header line of each file counts its entries and
those with a reason.

To rewrite a baseline after fixes, run the job's script by hand on linix with
`--write-baseline`, from a checkout of the commit:

    docker run --rm -v "$PWD":/src -w /src -e HM_EXCEPTIONS_FILE=.gitlab/security-exceptions.yml registry.gitlab.com/hmailserver/hmailserver/ci-linux:<tag>@sha256:<digest> bash build/ci/static-analysis.sh --write-baseline
    docker run --rm -v "$PWD":/src -w /src python:3.14-slim-trixie bash build/ci/semgrep.sh --write-baseline

`static-analysis.sh` fetches `master` from the checkout's origin, which a
worktree mounted into a container cannot reach: `HM_EXCEPTIONS_FILE` names the
file to judge by instead, and a GitLab job refuses it (exit 2). `semgrep.sh`
needs only the network. Or take a job's artifacts, which hold the tools' raw
output, and run `build/ci/security-findings.py --write-baseline` over them; its
help gives the command, and nothing is scanned again. Either way the rewrite
keeps every entry the run still finds, at its line now and with its reason,
drops the entries it no longer finds, and adds none: it prints each entry it
dropped and each finding it left out, with its fingerprint. `--reasons`, a JSON
file of `{"<fingerprint>": "<why>"}` and `{"rule:<rule>": "<why>"}`, gives a
reason to a kept entry that has none, and never replaces one. The job reports
how many of the baseline's findings it no longer sees.

Semgrep's registry packs change without a version. A Semgrep finding can
therefore be new because a rule is new, and not because the code changed. Each
run keeps the rules it fetched, with their SHA-256, in its artifacts
(`security/semgrep/rules/`), so two runs can be compared. The rules are under
the Semgrep Rules License v1.0, which allows this use but not redistribution.
That is why they are fetched and not committed.

What each scan leaves out, and why:

- **clang-tidy's checks** are chosen in `build/ci/clang-tidy.yaml`. The file
  runs `bugprone-*`, `cert-*`, `clang-analyzer-*` and `concurrency-*`, a few
  exclusions from each, and ten named checks. Each exclusion is written there
  with the count it had on a quarter of the tree. The families left out
  (`performance-*`, `modernize-*`, most of `readability-*`, `misc-*` and
  `cppcoreguidelines-*`, and `portability-*`) produced about 8,400 style
  findings on that quarter.
- **cppcheck** runs its error, warning and portability checks (`--enable=warning,portability`).
  Style and performance are left out for the same reason. The system headers
  are not read; library configurations stand for them.
  `build/ci/cppcheck-suppressions.txt` lists what is suppressed, with reasons.
- **Semgrep** does not read what `build/ci/semgrepignore` names: the regression
  suites, the tools' test projects and the two script harnesses (test code, as
  CodeQL's configuration left out too), the vendored QuickJS and zlib, and the
  WinForms designer's files. It does not read the C++, which is
  `static-analysis`'s. Without that file Semgrep would apply its own default,
  which leaves out `build/` and every `test/` directory without a word, so the
  script puts the file at the root of the checkout for the run. Semgrep also
  reads a file with no extension only when it is executable and starts with a
  shebang, and git checks `hmctl` out as an ordinary file. So the script copies
  each script with a shebang and no extension (`hmctl` and the packages'
  maintainer scripts) under a name with one, and reports what it finds against
  the original path.

### Dependency findings, the VEX document, and what OSV cannot answer

`dependency-scan` fails on a high or critical advisory that has a fixed
version, which is [SECURITY.md](../../SECURITY.md)'s threshold for "fixed before
the next release". A finding that does not affect the project is dismissed in
the VEX document, `.gitlab/hmailserver.openvex.json`, with its reason. Trivy
reads that document (`--vex`), and so does the OSV step, which dismisses an
advisory only where a statement names it, by its id or an alias, and names the
library at the version scanned by its purl (`pkg:generic/<name>@<version>`, the
purl `build/native-dependencies.ps1` gives the SBOM): a statement made for zlib
1.3.1 does not dismiss the same advisory for 1.3.2. The document is read, and
its products matched, by `build/ci/mr-dependency-check.py`, which the change
gate uses too. A record with no CVSS v3 vector and no rating of its database's
is scored by its CVSS v4.0 vector with that script's calculator, so that
CVE-2026-76844, in zlib's `gz_write`, is High (8.3) with a fixed version; the VEX
document says zlib 1.3.2 is not affected, since `gzwrite.c` is not vendored. A
dismissed finding is still listed, marked as dismissed.

OSV's data on C and C++ libraries is partial. So each vendored library is also
asked about a *control* version, one a published advisory is known to affect.
When OSV has nothing for the control either, the answer for the real version is
UNKNOWN (a skipped test case), not clean. On 25 September 2026:

| Library | Version | OSV's answer |
|---|---|---|
| OpenSSL | 4.0.2 | no advisory (4.0.0, the control, has 29) |
| Boost | 1.92.0 | two OSS-Fuzz crash reports, OSV-2024-112 (Boost.Graph's graphviz reader) and OSV-2024-914 (Boost.Regex's formatter), neither with a fixed version. OSV's Boost records are OSS-Fuzz's only, and Boost's CVEs are not among them |
| zlib (vendored) | 1.3.1 | CVE-2026-22184 (contrib/untgz, which is not vendored), CVE-2026-27171 (CPU use in `crc32_combine64`, fixed in 1.3.2) and CVE-2026-3381 (Perl's Compress::Raw::Zlib, attached to zlib's repository) |
| MariaDB Connector/C | 3.4.9 | no advisory (3.1.7, the control, has one) |
| libpq | 18.3 | UNKNOWN: OSV names nothing for 17.2 either, which CVE-2025-1094 affects |
| QuickJS | 2025-04-26 | UNKNOWN: OSV has no QuickJS data |
| 7-Zip (`7za.exe`) | 19.00 | UNKNOWN: 19.00 predates the upstream repository's first tag (22.01), so OSV cannot match it. The two advisories OSV has for 22.01 (CVE-2025-55188, CVE-2026-58052) reach back to the first commit. CVE-2023-31102 (a crafted 7z archive and the PPMd decoder, before 23.00) is in OSV only with a range it could not resolve, so no query by version finds it |
| SQL Server Compact | 4.0 SP1 | UNKNOWN: no open database tracks it, and Microsoft ended its support in July 2021 |

The CI image's scan treats `linux-libc-dev` as a header-only package: the rule
`HEADER_ONLY` in `build/ci/dependency-scan.py`, a list of the image's packages
that install headers alone and carry no code, with the reason for each. That
package is the kernel's user-space API headers, which `build-essential` pulls in
and which hold no kernel code; Trivy files every advisory against the kernel's
source package under it, and the image holds no kernel, since its jobs run on
their runner's. An advisory against it in the image's scan is listed in every
report - `findings.txt`, the JUnit text and the GitLab report, whose record
carries the status in its `details`, under the record's own id, so that the
dashboard shows it - with the status "not affected: header-only package, the
advisory is against the kernel it was built from"; the image's test case counts
those apart from the image's other packages, each by severity, with how many of
them would otherwise have held the release; and none of them is counted among
the high or critical with a fixed version, by the scan or by
`build/ci/security-gate.py`, which reads the same rule off the record's package
and `location.file` and says in the release job's log what it spared. The rule
reaches that name and no other (`libc6`, `linux-libc-dev-arm64-cross`) and the
image's scan alone: the same name in a lock file would count as any package;
both scripts' self-tests hold it. On 6 October 2026 (`master`'s pipeline at
`a5a02b3be`) the image had 860 packages, 316 of them Ubuntu's, and 3,608
advisories - 1 Critical, 81 High, 3,365 Medium and 161 Low - of which 3,535 were
against `linux-libc-dev` (1 Critical, 81 High, 3,323 Medium, 130 Low) and 73
against the image's other packages (42 Medium, 31 Low); none High or Critical
with a fixed version, and the maintainers dismissed the 3,535 on the dashboard
as not applicable with this reason. By 8 October Ubuntu had published
`linux-libc-dev` 7.0.0-38.38, and `batch44`'s pipeline 2924313539 counted 60 of
the kernel's advisories against the image's 7.0.0-34.34 as High or Critical with
a fixed version (3,610 in all, 15 dismissed by the VEX document): the next
release would have been held on headers, which is what the rule is for. On 25
September 2026 there were 259 packages and 3,024 advisories, 2,953 of them
against `linux-libc-dev`. A rebuild of the image (`build/ci/build-linux-image.sh
--push`, [The image](#the-image)) takes Ubuntu's fixes when they come, as the 8
October rebuild did.

### The repository checks, and the Scorecard checks left out

`repo-hygiene` (`build/ci/repo-hygiene.py`) reads GitLab's API without a token.
It never sends one. Each check answers PASS, FAIL or UNKNOWN. UNKNOWN means
the answer is not public, and it never counts as a pass. On 25 September 2026:

- **Branch-Protection** passed. `master` and `batch*` are protected, and only
  Maintainers push or merge. `master` refuses force pushes, while `batch*`
  allows them, because a batch branch is re-cut while it is folded. All 44 `v*`
  tags are protected tags. Who may create one is a setting GitLab does not show
  anonymously.
- **Signed-Releases** passed. The five newest `v*` releases carry Sigstore and
  cosign bundles, two of them in-toto provenance as well. git verified the SSH
  signature on each of their tags against the allowed signers file (then
  `.github/allowed_signers`, now `.gitlab/allowed_signers`).
- **Security-Policy** passed. `SECURITY.md` offers a confidential issue and an
  address.
- **Pinned-Dependencies** failed on four images: `python:3.13-slim-trixie` and
  `node:22-trixie-slim` in the `checkers`, `harnesses`, `sign-off` and `release`
  jobs are named by tags that can move. Everything else passed: every Dockerfile
  `FROM`, every action a GitHub workflow then kept in the tree used (by commit,
  except the SLSA generator, which must be called by its tag and which Scorecard
  exempts too; the workflows have since been removed),
  every install in the CI scripts, and a lock file beside every NuGet project.
  The check also notes three Python modules that a script imports and that no
  requirements file names: PyYAML, Playwright and jsonschema.
- **Binary-Artifacts** passed. None of the tracked files is an executable or
  compiled file, judged by its content as well as its name.
- **Token-Permissions** is UNKNOWN. What the tree can show passed: every job on
  linix or bench150 extends `.own-runners`; no CI script reads `CI_JOB_TOKEN` or
  any `*TOKEN*` or `*SECRET*` variable; `CI_DEBUG_TRACE` is set nowhere. The
  only credential written in the file is the throwaway password of the Linux
  suite's own PostgreSQL service (on 25 September; see below for the first
  pipeline). The project's job-token allowlist and who may
  override pipeline variables are not public. A maintainer's `glab api
  projects/86730559` read `ci_job_token_scope_enabled: false` (the allowlist,
  "Limit access to this project", is off) and
  `ci_pipeline_variables_minimum_override_role: no_one_allowed`.

On the first pipeline, 26 September 2026, two checks failed. Pinned-Dependencies
failed on six images named by a tag, the shared runners' Python and Node
images; each is pinned now to the digest Docker Hub served for its tag that
day, with the tag kept in the reference for Renovate. Token-Permissions failed
on the `renovate` job, on linix without `.own-runners`, and on
`MARIADB_ROOT_PASSWORD`, written in `.gitlab-ci.yml` for the upgrade gate's
service. The MariaDB service now starts with `MARIADB_ALLOW_EMPTY_ROOT_PASSWORD`,
a switch rather than a credential, reachable from its own job alone. And the
check itself asked the wrong question: whether a job extends `.own-runners`, which
a job with rules of its own (the `renovate` schedule, the release chain's jobs
on a protected `v*` tag) does not need and a job overriding its rules would
pass. It now reads the rules each job runs by, its own or the first it extends,
with `!reference` followed, in `.gitlab-ci.yml` and the files it includes: a job
on linix or bench150, a job with `id_tokens` or `secrets`, and every job that
runs a CI script handling a credential must ask for a protected ref in every
rule that creates it, and shut a merge request out. A value on a `*PASSWORD*`,
`*TOKEN*` or `*SECRET*` key fails unless it is PostgreSQL's throwaway password,
one of MariaDB's two switches, or a variable reference.

"Asks for a protected ref" means a clause every pipeline the rule admits must
meet: `$CI_COMMIT_REF_PROTECTED == "true"` joined by `&&` to the rest of each
alternative, not inside an `||` or a comment (a condition written over several
lines is read whole). The check reads the CI files without a YAML library, so
what it cannot read it refuses rather than passes: a rules list given inline or
by an alias, a merge key (`<<:`), an `extends` by an alias, an `!reference` the
files do not resolve, and an include that is not a local file or one of GitLab's
templates. Tags and `extends` may be block lists. A job is on the project's
runners when it asks for a tag that is a variable, or for any tag they are
registered with (`linix`, `linux` and `docker` for linix and server2;
`bench150`, `bench` and `windows` for bench150); a job that names none takes
`default:`'s. One whose tags are `default:`'s `[$HM_RUNNER]` is judged by the
workflow's rules, which choose its runner (since 9 October 2026, [Every job on
the project's own runners](#every-job-on-the-projects-own-runners)): a rule that
sets `HM_RUNNER` to GitLab's hosted runner makes nothing for the project's, and
every other rule that makes a pipeline must ask for a protected ref and shut out
a merge request, as a job's own rules must; a job that sets `HM_RUNNER` itself
is judged by its own. `id_tokens`, `secrets` and a credential in a template
count for every job that extends it or takes a part of it by `!reference`.
`python3 build/ci/repo-hygiene.py --self-test`, which the `checkers` job runs,
holds the check to CI files made to pass it and to 30 made to break it (23 at
first); the check as first rewritten (a4636e983) passed 22 of those 23, and
failed two of the eight that should pass. The runners are registered for
protected refs only (`access_level` `ref_protected`, read by a maintainer with
`glab api runners/<id>`: linix and bench150 on 26 September 2026, and those two,
`linix-systemd` and server2 on 6 October 2026), so none of that was open on this
project: the check is the CI files' own guarantee.

Left out of Scorecard's checks, each for a reason:

- **Code-Review.** Up to 6.3.4 the maintainers committed to `master` without
  merge requests, which was then the project's rule. From the first change
  after 6.3.4 every change lands through a merge request that the other
  maintainer approves ([GOVERNANCE.md](../../GOVERNANCE.md), Review), and the
  approvals are on the merge requests; GitLab's approval rules are a paid
  feature, so no project setting records the rule for a check to read.
- **Maintained** and **Contributors.** They measure activity and outside
  organisations, not the repository's settings.
- **CII-Best-Practices.** It looks up an OpenSSF Best Practices badge. The
  entry made for the GitLab home, 14949, has held the passing badge since
  26 September 2026, which the README's badge shows; the first entry, 14187,
  was made through a GitHub login and can no longer be edited.
- **Webhooks.** A project's webhooks are readable only with a maintainer's
  token.
- **Dangerous-Workflow.** It looks for GitHub Actions' `pull_request_target` and
  its `${{ }}` injection, which GitLab CI does not have. The same risk is covered
  here by the own-runner rules, and by SECURITY.md's rule that a branch or tag
  name reaches a command through the environment only.
- **Packaging**, **SAST** and **CI-Tests.** They read GitHub's checks API. SAST
  is this section.
- **Fuzzing.** The harnesses are in `fuzz/`, and the nightly job stopped with
  the account (Fuzzing.md); on GitLab `linux-fuzz` has run them under its
  own schedule since 28 September 2026 ([The fuzz schedule](#the-fuzz-schedule)).
- **License.** `LICENSE` is at the root.
- **Dependency-Update-Tool.** It could be answered, and it would fail, as the
  table above says.

The OpenSSF API's published score, and the badge it serves, are GitHub-only: the
API accepts a result only from a GitHub Actions run. Since 6.3.6 the project runs
Scorecard itself and publishes the result on its own site ([OpenSSF
Scorecard](#openssf-scorecard)); `repo-hygiene` stays, on every protected
pipeline, and answers Token-Permissions, which Scorecard does not on GitLab.

### OpenSSF Scorecard

`scorecard` (`.gitlab/ci/scorecard.yml`, `build/ci/scorecard.sh`) runs OpenSSF
Scorecard over the project once a week, on linix, in the pipeline of the
schedule on `master` described `scorecard`, and in no other. The same pipeline's
`pages` and `package-repos` then publish its result on the update feed's site:

- https://updates.progressiverobot.com/scorecard/scorecard.json, Scorecard's
  result as it wrote it (`--format=json --show-details`): each check, its score,
  its reason and what it found;
- https://updates.progressiverobot.com/scorecard/badge.svg, a badge with the
  aggregate score, made from that file by `build/ci/scorecard-pages.py`.

Every later deployment of the site - a push to `master`, a release, the feed's
schedule - reads the result the site serves, checks that it is Scorecard's, and
publishes it again with a badge made afresh, so a deployment never drops it; a
result it cannot fetch or read is a warning in `package-repos`' log and no
`scorecard/`, never a failed deployment. The README's badge is that SVG, linked to the JSON.

**Where Scorecard comes from.** Its releases, its `ghcr.io` image and Homebrew's
bottles are on GitHub; its `gcr.io/openssf/scorecard` image answered "This API
method requires billing to be enabled" on 25 and again on 28 September 2026;
Debian and Ubuntu do not package it. Nixpkgs builds it, and `cache.nixos.org`
serves the build signed with the `cache.nixos.org-1` key that Docker Hub's
`nixos/nix` image trusts. The job runs in that image (2.35.2, pinned by digest)
and realises one store path,
`/nix/store/d3zxf860sbva21lgynwsadsdp4bsh9xv-scorecard-5.5.0`: Nix fetches it and
its closure (glibc, tzdata, mailcap, iana-etc) from `cache.nixos.org` alone and
checks each path's signature and hash. The store path is input-addressed - it
names the recipe, and the bytes are whatever the key signs - so the script pins
the NAR hash as well, as `cache.nixos.org`'s narinfo gave it on 28 September 2026
(`sha256:0brx32y7m1z68clfasdp3rypih6l5r0liy2bh21jmds4q6fvkipj`), and runs nothing
when `nix-store --query --hash` names another: the two together pin the build as
a digest pins an image. Its `version` says v5.5.0, commit
`c395761df6afe1a69e476bc60a013a94bcbc153f`. On 28 September 2026, in that image
on the maintainer's laptop with GitHub's hosts at 0.0.0.0, the closure realised in 4 seconds,
and an anonymous run over two public checks of `gitlab.com/Progressiverobot/hmailserver`,
the project's path before its move, answered (10 of 10 for License and
Security-Policy). A new version is a new store path and NAR hash, the path read
from `hydra.nixos.org`'s job
`nixpkgs/unstable/scorecard.x86_64-linux` and the hash from its
`https://cache.nixos.org/<hash>.narinfo`,
never a `nixpkgs#` flake reference, whose registry names GitHub. The job keeps
GitHub out of itself as well ([Nothing from GitHub, in the job](#nothing-from-github-in-the-job)):
the image has no `/etc/nsswitch.conf`.

**What it checks on GitLab** (its README, v5.5.0): Binary-Artifacts,
Branch-Protection (some settings only with a maintainer's token) and CI-Tests
are supported; Code-Review, Contributors, Fuzzing, License, Maintained,
Pinned-Dependencies, Packaging, Security-Policy, Signed-Releases and
Vulnerabilities are "validating"; Dangerous-Workflow, Dependency-Update-Tool,
SAST and Token-Permissions are not answered. CII-Best-Practices looks the badge
up by the repository's address, so entry 14949's `repo_url` must follow the
project's move into the group on 29 September 2026: the owner changes it to
`https://gitlab.com/hmailserver/hmailserver`.

**The token, and the one step that is the owner's.** Scorecard reads a GitLab
project through the API with `GITLAB_AUTH_TOKEN`, which must carry `read_api`,
`read_user` and `read_repository`. `read_user` exists for a personal access
token only, so a project or group token cannot serve, and the token is the
owner's:

1. **The owner** makes a personal access token (*Preferences*, *Access tokens*)
   with exactly `read_api`, `read_user` and `read_repository`, expiring in a year.
2. It is stored as the project's CI/CD variable `GITLAB_AUTH_TOKEN`: protected,
   masked and hidden, with the environment scope `scorecard`. The job names that
   environment (`action: access`, which records no deployment) and no other job
   does, so no other job of a protected pipeline is given the token. The script
   never writes it: Scorecard reads it from the environment, its own messages go
   to the job log, where GitLab masks it, and the artifact holds the result
   alone.
3. A pipeline schedule on `master` described exactly `scorecard`, weekly, with no
   variables (the project refuses a schedule's), made by a maintainer.

Until the variable exists the job says "not configured", exits 0 and publishes
nothing, and the site has no `scorecard/`. The token expires in a year: a
refused token makes Scorecard write no result, and the job fails, saying so,
rather than publish an empty one.

**Why it is self-published.** `api.scorecard.dev` accepts a result only through
`POST /projects/{platform}/{org}/{repo}` from GitHub's Scorecard action, with an
OIDC token GitHub issues; its weekly scan reads GitHub projects only, and adding
one to its list is a pull request on GitHub. So neither the OpenSSF API nor
deps.dev, which shows the API's scores, can carry this project's. The site's
copy is the project's own, run by its own CI from a pinned build, and says so.

### SonarQube Cloud

SonarQube Cloud (sonarcloud.io) analyses the project from outside the pipeline.
The organization `hmailserver` is bound to the GitLab group `hmailserver`, in
SonarQube Cloud's EU region, on its OSS plan - free for public projects under
an OSI licence, with branch and merge request analysis and custom quality
gates; the plan can be chosen only when an organization is made. The project is
imported now that it has moved into the group (29 September 2026) - the owner's
step - and from the import on SonarQube
Cloud's **automatic analysis** runs, from the webhook the import makes: on every
push to the default branch and on every merge request, on SonarQube Cloud's own
machines. It leaves a summary comment on the merge request; GitLab gets no
inline comments from it.

Automatic analysis rather than a CI job, for four reasons:

- **no secret in a merge request's pipeline.** A CI-based scan of a merge
  request would hold `SONAR_TOKEN` in a pipeline that runs code nobody has
  reviewed, which `repo-hygiene`'s Token-Permissions check refuses;
- **no runner time.** linix already carries the longer share of a protected
  pipeline;
- **every file.** It reads the Windows-only sources - the COM API, the service,
  the ADO and SQL Server Compact backends, the registry - which cppcheck and
  clang-tidy never see (they read what the Linux build's compile database
  lists), and which nothing had analysed since CodeQL stopped on 18 September
  2026; and the C# of the Control Panel and the tools, without the Windows
  build;
- **nothing to fetch.** It fetches nothing in any job, from GitHub or anywhere.

The price: SonarQube Cloud says automatic analysis may miss some issues that an
analysis from a compilation database finds, and its precision on the
Windows-only files, analysed with no MSVC headers, is not documented. A project
uses one mode or the other: a CI-based analysis needs automatic analysis turned
off first (*Administration*, *Analysis Method*), a token, and the scanner run in
the CI image with `static-analysis.sh`'s compile database, for protected refs
only.

**The scope** is `.sonarcloud.properties` at the root: the whole tree as
sources, less the third-party code under `libraries/`, the vendored QuickJS and
zlib, the generated tables (`HelpPagesData.cpp`, `PortalPageData.cpp`,
`PortalLanguagesData.cpp`, `PortalSmimeData.cpp`, `PortalOpenPgpData.cpp`,
`ProfilesData.*`, `SettingsDefaults.h`, `PublicSuffixListData.h`,
`SettingsSearchIndex.g.cs`) and the WinForms designer's files; the regression
suites, the tools' test projects, the fuzz harness and the two page harnesses as
tests; C++14 as the standard its C++ rules report against, which is what MSVC
compiles the shared sources as; Python 3.14. `sonar.sources` and `sonar.tests`
take plain paths, no wildcards; the exclusions take patterns. Automatic analysis
reads the file from the default branch only, and a value in the file replaces
the same one set in the project's settings. So until 6.3.6 reached `master`,
the exclusions were to be set in the project's settings as well
(*Administration*, *General Settings*, *Analysis Scope*, or `api/settings/set`),
with the test trees among them, since `sonar.tests` cannot be set there: until
the file landed, the tests were to be left out rather than read as tests. The
file has been on `master` since, so its values replace any the settings hold.

**What it holds, and what it cannot.** On GitLab's free tier a merge request
cannot be made to wait for SonarQube Cloud's quality gate (external status
checks are GitLab Ultimate), so its verdict is the summary on the merge request
and the badge in the README; `semgrep-sast-gate` is the SAST that holds a
merge request. Its findings are triaged in SonarQube Cloud itself: a Blocker or
Critical bug or vulnerability is fixed, or marked there as a false positive or
accepted, with the reason.

### What GitLab gives on the free tier, and what Ultimate would

The project is on GitLab's free tier (its namespace's plan read `free` on 25
September 2026). What each feature this page touches needs, by
docs.gitlab.com on 28 September 2026:

| Feature | Tier | Here |
|---|---|---|
| SAST (`semgrep-sast`), its report stored with the job | Free | Yes; `semgrep-sast-gate` reads it |
| Pipeline secret detection | Free | Yes, and it fails a pipeline |
| JUnit reports, the pipeline's *Tests* tab | Free | Yes, every scan and suite writes one |
| Code Quality: a job's `codequality` report in the merge request widget | Free (the pipeline's Code Quality tab is Premium; the diff view and the project summary Ultimate) | No: see below |
| Coverage in a merge request's diff (Cobertura) | Free | No: the coverage job is weekly, not per merge request |
| Project badges | Free | Yes |
| Merge request approvals, optional | Free: they "don't prevent merging" | Yes; the landing script requires them |
| Required approval rules, code owners' approval | Premium, Ultimate | No |
| External status checks (a merge request waiting on SonarQube Cloud) | Ultimate | No |
| The Security tab, the merge request security widget, the vulnerability report | Ultimate | No; the same reports would appear there unchanged |
| Dependency scanning, the dependency list, `reports:cyclonedx` | Ultimate | No: `dependency-scan` and `mr-dependency-check` instead |
| Licence scanning of CycloneDX files | Ultimate | No |
| Secret push protection | Ultimate | No: the hooks, the landing's scan and `secret_detection` instead |
| Container scanning's results in the vulnerability report | Ultimate (the job runs on Free) | The job, in a release's pipeline |

**GitLab for Open Source** grants Ultimate and 50,000 compute minutes a year to
a qualifying namespace: every project in it under an OSI-approved licence,
public, and an organisation that does not "seek to make a profit by selling
services, by charging for enhancements or add-ons, or by other means". Whether
Progressive Robot Ltd meets that last clause is the owner's to say; if it does,
the application is for the group `hmailserver`, where the project has been
since 29 September 2026. With Ultimate,
and no change to a job, the rows above marked Ultimate come on: the Security
tab and the vulnerability report for the reports already uploaded, required
approvals, external status checks for SonarQube Cloud's quality gate, and push
protection.

**Why GitLab's Code Quality widget is not used.** On the free tier the widget
shows the findings of a `codequality` report written by a job of the merge
request's own pipeline, against the target branch's. The scans that report on
code quality here, `static-analysis` and `semgrep`, run on linix for protected
refs only and never in a merge request's pipeline, so they cannot feed it; the
one scan that does run there, `semgrep-sast`, is already read and enforced by
`semgrep-sast-gate`, and a second rendering of the same findings would hold
nothing. GitLab's own Code Quality template (CodeClimate's engines, run under
Docker-in-Docker) was deprecated in GitLab 17.3 and is removed in 19.0, and
SonarQube Cloud does not write GitLab's format; it comments on the merge request
itself. A job that turned `security-findings.py`'s findings into a
`codequality` report in merge request pipelines is possible, if the widget is
ever wanted.

### What else was left out, and why

- **CodeQL itself.** Its command-line tool and its query packs are published
  only on GitHub: the tool as release assets on github.com, the packs on
  `ghcr.io`.
- **OSV-Scanner.** It is published on GitHub and `ghcr.io` only. Neither Debian
  nor Ubuntu packages it (apt knows no `osv-scanner` in trixie or 26.04). The
  OSV step calls OSV's API directly instead.
- **Scorecard's own channels.** Its one channel that is not GitHub,
  `gcr.io/openssf/scorecard`, answered "This API method requires billing to be
  enabled" on 25 and 28 September 2026, and the others are GitHub's release
  assets and `ghcr.io`. Since 6.3.6 the tool comes from `cache.nixos.org`
  instead ([OpenSSF Scorecard](#openssf-scorecard)).
- **pip-audit and npm audit.** Neither has anything to read. The Python tools
  use the standard library and declare no dependency (the three that import
  more are named above), the one Python lock file in the tree is the scanners'
  own, which Trivy reads, and there is no `package.json` in the tree: the Node
  harnesses install nothing.
- **A weekly scheduled scan.** A schedule would start every job whose rules
  allow a protected ref, the Windows gate included. A schedule for the scans
  alone needs the other own-runner jobs to skip one, and those are not this
  row's blocks.

### What they cost

The five jobs add 35 to 50 minutes of work for `static-analysis` (clang-tidy's
static analyser is most of it) and about ten for the rest to linix's share of a
protected pipeline. linix runs up to three jobs at once ([linix](#linix)), so they
run beside the build and the suite rather than after them, but on the same
sixteen cores: they lengthen the suite rather than queue behind it. linix's
share may so become the longer of the two lanes, against the Windows gate's 2.5
to 3 hours on bench150. If it does, the first lever is
`static-analysis` on `master` and `v*` only. A batch's pipeline
runs again on `master` when the batch lands, so a new finding is still seen
there. Making that change is one `rules:` line.

Since 6.3.6 `linux-build-gcc` adds a second whole build to linix's share of a
`master` or `batch*` pipeline, never of a tag's: 25.5 minutes in WSL at four
jobs, and not yet measured on linix, whose job has six CPUs. `scorecard` takes
a few minutes of linix once a week, in a pipeline of its own. The change checks
run where the other checks do: on linix or server2 in a `batch*` branch's
pipeline, and on GitLab's hosted runner, on the namespace's compute minutes, in a
merge request's; the dependency check runs only when a lock file changed.

What is still done by hand
--------------------------

- For any branch that is not protected, everything the project's machines run:
  the builds and both suites, the .NET build, tests and `dotnet format`, the
  packages, the upgrade gate and the census. A contributor runs what applies
  before opening a merge request, and the maintainer runs it before a change
  lands, as [CONTRIBUTING.md](../../CONTRIBUTING.md) describes.
- `check-package-manifests.py` against a released installer, and the two
  PowerShell checks CI does not run: `check-schema-versions.ps1`, and
  `check-db-scripts.ps1`, which needs Windows PowerShell 5.1 and SQL Server
  Compact.
- The Linux build on AArch64 hardware; the GCC build is `linux-build-gcc` since
  6.3.6 ([The GCC build](#the-gcc-build)), and
  `bash build/linux-tu-census.sh --cc g++` is still the GCC census by hand. The AArch64
  packages and AppImage are built by the package jobs of
  `.gitlab/ci/packages-*.yml` and run there under qemu-user on linix; running
  them on AArch64 hardware is still by hand.
- The Windows ARM64 build: the ARM64 libraries, `build.ps1 -Platform ARM64`,
  `build-tools.ps1 -Platform ARM64` and `hMailServerARM64.iss`, until
  `windows-packages-arm64` gets past its toolchain check on bench150: until
  then that job stops, allowed to fail, and names what is missing. bench150
  has Visual Studio's ARM64 tools and ATL for ARM64; in pipeline 2887115157
  the check stopped the job on the ARM64 C++ runtime, which until 28 September
  2026 it could not find on any machine ([PackagesWindows.md](PackagesWindows.md),
  "The ARM64 toolchain" and "What the owner must do"). No runner is an ARM64
  Windows machine to test it on ([WindowsOnArm.md](WindowsOnArm.md) has the
  commands).
- The release's two buttons, `release` and `release-publish`
  ([The release](#the-release)), and pushing the image and the package
  managers' manifests, which are still the Roadmap2 row "The release chain off
  a single forge".
- The installer smoke test, which runs from the VMware host with
  `build/ci/installer-smoke-run.sh`. A batch runs it before it lands, and a
  release runs it before it is published (see [The installer smoke
  test](#the-installer-smoke-test)).
- Building the Linux job image, and pushing it to the project's registry, when
  its Dockerfile changes (see [The image](#the-image)), and creating the fuzz
  schedule once (see [The fuzz schedule](#the-fuzz-schedule)) and the Scorecard
  schedule and its token (see [OpenSSF Scorecard](#openssf-scorecard)).
- Bringing a runner back when its machine has been rebuilt (see
  [Re-creating a runner](#re-creating-a-runner)).
