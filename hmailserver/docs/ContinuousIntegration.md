Continuous integration
======================

GitLab CI starts every pipeline (`.gitlab-ci.yml` at the repository root). The
cheap checks run on GitLab's shared runners, for every push and every merge
request. The builds, both regression suites and everything else that needs a
machine of the project's run on its own two machines, and only for the refs that
only a maintainer can push. The owner decided this on 25 September 2026, a week
after GitHub suspended the project's account and its workflows with it. The same
day, the rest of the old GitHub workflows were ported
([From GitHub Actions to GitLab CI](#from-github-actions-to-gitlab-ci)), and a
nightly fuzz pipeline was added ([The fuzz schedule](#the-fuzz-schedule)).

**The jobs have run since 26 September 2026.** The first pipeline, 2884863496
on `master`, failed eleven of its jobs ([What the first pipeline found](#what-the-first-pipeline-found));
`master`'s pipeline 2886389460 on 27 September passed 25 of its 29, failed
`secret_detection`, `semgrep` and `static-analysis`, and left `db-matrix`
manual. This page was written before either runner had taken a job: the ported
Linux jobs' times below are those of their scripts run by hand on linix, in the
jobs' own images and beside the same services, and every other duration is an
estimate made from the gates run by hand, and says so. A pipeline's own times
are on its jobs' pages.

What runs where
---------------

| Job | Stage | Runs on | Runs for | What it does |
|---|---|---|---|---|
| `checkers` | check | GitLab's shared runners | every pipeline but a schedule's | Every Python checker, the self-tests, the generators' `--check` ([CONTRIBUTING.md](../../CONTRIBUTING.md), "The checkers") |
| `harnesses` | check | GitLab's shared runners | every pipeline but a schedule's | The Control Deck's and the webmail's script harnesses, and the S/MIME and OpenPGP checks |
| `sign-off` | check | GitLab's shared runners | every merge request, and a `batch*` branch | Every commit the merge request or the batch adds to `master` carries its author's `Signed-off-by` (`build/sign-off.py`; [Landing on master](#landing-on-master)) |
| `editorconfig` | check | GitLab's shared runners | every pipeline | Every text file against `.editorconfig`, and a negative control |
| `chart-and-compose` | check | GitLab's shared runners | a change to the Helm chart or the Compose file | The chart linted and rendered, the Compose file resolved |
| `update-helper` | check | GitLab's shared runners | a change to the update helper or the units | `build/test-hmailserver-update.sh` under dash, and `systemd-analyze verify` of the three units |
| `package-templates` | check | GitLab's shared runners | a change to the winget or Chocolatey templates | The templates render and agree, checked offline |
| `roadmap` | check | GitLab's shared runners | a change to `Roadmap.md` | `build/check-roadmap.ps1` |
| `cli-packages` | check | GitLab's shared runners | a change to the command-line clients or their packaging, and never a tag | `hmailserver-cli`'s sdist and wheel built twice and compared, held to the tree, `twine check`ed and installed alone (`.gitlab/ci/cli-packages.yml`; [CommandLinePackages.md](CommandLinePackages.md)) |
| `cli-powershell` | check | GitLab's shared runners | the same | The PowerShell module's manifest and PSScriptAnalyzer from the Gallery, under pwsh (`build/ci/cli-powershell.ps1`) |
| `linux-build` | build | linix | a protected ref | The Linux server with clang, every `*-tests` program the CMake build makes, the OpenPGP writer against GnuPG, and the `.deb` and the `.rpm` (`build/ci/linux-build.sh`) |
| `pages` | release | GitLab's shared runners | `master`, a `v*` tag, a schedule on `master` | Renders the update feed from the project's releases into `public/`; it deploys nothing itself (`pages: false`, from `.gitlab/ci/package-repos.yml`) (see [The update feed](#the-update-feed)) |
| `package-repos` | release | GitLab's shared runners | wherever `pages` runs, after it | Adds the signed apt and dnf repositories to `pages`' `public/`, or, without the signing key, nothing, and GitLab Pages publishes the result at https://updates.progressiverobot.com ([PackageManagers.md](PackageManagers.md), "apt and dnf") |
| `linux-suite` | test | linix | a protected ref | The whole of `LinuxRegressionTests` against that server on PostgreSQL 16 (`build/ci/linux-suite.sh`) |
| `linux-cross` | test | linix | a protected ref | Every core translation unit compiled for AArch64 (`build/ci/linux-cross.sh`) |
| `linux-sanitize` | test | linix | a protected ref | `snmp-tests` under AddressSanitizer; says SKIPPED and passes while the tree has no `HM_SANITIZE` (`build/ci/linux-sanitize.sh`) |
| `linux-db-upgrade-pgsql`, `linux-db-upgrade-mysql` | test | linix | a protected ref | A database made by 6029's own create script, with orphaned rows, upgrades to this schema and to the schema of a new database, on PostgreSQL 16 and MariaDB 11 (`build/ci/linux-db-upgrade.sh`) |
| `linux-package-deb`, `linux-package-rpm` | test | linix | a protected ref | The packages installed in clean Ubuntu 26.04 and Fedora 44 containers; the `.deb` configured, started and asked for the Deck (`build/ci/linux-package-install.sh`) |
| `linux-image` | test | linix | a protected ref | The container image built from the `.deb` with kaniko and inspected, not pushed (`build/ci/linux-image.sh`) |
| `linux-sbom` | test | linix | a protected ref | The SPDX and CycloneDX SBOMs of the tree, kept 90 days (`build/ci/linux-sbom.sh`) |
| `linux-repro` | test | linix | a protected ref, after `linux-build` | `linux-build`'s binary, `.deb` and `.rpm` made a second time from a copy of the tree at another path with other file modes, in another build directory, started under another umask and in another time zone, and compared: all three must be byte for byte identical (`build/ci/linux-repro.sh`; [ReproducibleBuilds.md](ReproducibleBuilds.md)) |
| `db-matrix` | test | linix | a protected ref, started by hand | The `linux-build` server against MySQL 8.4, MariaDB 11.4 or PostgreSQL 17 in the test lab (`HM_LAB_DB`), the database-sensitive fixtures or the whole suite (`HM_SUITE_FILTER`); `build/lab/db-suite.sh`, [TestLab.md](TestLab.md) |
| `windows-gate` | test | bench150 | a protected ref | The whole Windows suite in the bench's own checkout (`build/ci/windows-gate.ps1`) |
| `windows-dotnet` | test | bench150 | a protected ref | The Control Panel and the tools built with warnings as errors, published and tested, and `dotnet format` (`build/ci/windows-dotnet.ps1`) |
| `linux-fuzz` | test | linix | the schedule described `fuzz`, and nothing else | The three MIME harnesses under libFuzzer (`build/ci/linux-fuzz.sh`); its findings are its artifact, for Developers and above |
| `linux-coverage` | test | linix | the schedule described `coverage`, and nothing else | The server and its tester programs built with clang's source-based coverage, the testers and the whole Linux suite run against it, and llvm-cov's line, region and branch figures (`build/ci/linux-coverage.sh`, [The coverage schedule](#the-coverage-schedule)) |
| `static-analysis` | test | linix | a protected ref | cppcheck and clang-tidy over the server's C++, held to a baseline ([Security scanning](#security-scanning)) |
| `semgrep` | test | linix | a protected ref | Semgrep's open rules over the C#, Python, JavaScript and shell sources, held to a baseline |
| `ci-image-packages` | test | linix | a protected ref | Copies the CI image's package database out, for `dependency-scan` |
| `dependency-scan` | test | linix | a protected ref | Trivy over the lock files and the CI image, OSV over the vendored libraries |
| `repo-hygiene` | test | linix | a protected ref | The repository checks OpenSSF Scorecard ran, answered from GitLab and the tree |
| `semgrep-sast` | test | GitLab's shared runners | `master`, and a merge request | GitLab's own SAST template: its Semgrep analyzer and rules over the C, C++, C#, Python and JavaScript, with no baseline (`.gitlab/ci/gitlab-security.yml`) |
| `secret_detection` | test | GitLab's shared runners | `master`, a merge request, and the schedule described `security` | GitLab's own secret-detection template: a key, token or password in the commits the pipeline brings, or in the whole history for the `security` schedule; a finding `.gitleaksignore` does not name fails it (`.gitlab/ci/gitlab-security.yml`) |
| `windows-installer` | release | bench150 | a `v*` tag, after `windows-gate` | The Windows installer, unsigned, built in the bench's checkout (`build/ci/windows-installer.ps1`; [The release](#the-release)) |
| `release` | release | GitLab's shared runners | a `v*` tag, started by hand | Checks the tag, the version stamp and the release settings, reads the four security scans' reports (`build/ci/security-gate.py`), collects the fifty-five files a release carries before the installer's SBOM (eight in 6.3.5), and counts what GitLab and Pages will take of it (`build/ci/release-chain.py collect`) |
| `release-authenticode` | release | bench150 | a `v*` tag, after `release` | The installer Authenticode-signed through Azure Artifact Signing with the job's ID token (`build/ci/windows-authenticode.ps1`) |
| `release-sbom` | release | GitLab's shared runners | a `v*` tag, after `release-authenticode` | The signed installer's own SBOM, `<installer>.cdx.json`, and the statement of the release's SLSA provenance (`build/ci/release-chain.py sbom`, `build/asset-sbom.py`) |
| `release-sign` | release | GitLab's shared runners | a `v*` tag, after `release-sbom` | Every file signed with Sigstore as this pipeline, each primary file's bundle copied to `<file>.sigstore.json`, and the provenance attested (`build/ci/release-sign.sh`) |
| `release-verify` | release | linix | a `v*` tag, after `release-sign` | Every bundle checked with this commit's `hmailserver --verify-update`, and the SBOMs, the copies and the provenance held to the files (`build/ci/release-verify.sh`) |
| `release-publish` | release | GitLab's shared runners | a `v*` tag, started by hand after `release-verify` | The files uploaded to the package registry and the GitLab release created (`build/ci/release-chain.py publish`) |
| `update-feed` | release | (a trigger) | a `v*` tag, after `release-publish` | Starts a downstream pipeline on `master`, passing no variables, which runs `pages` and `package-repos` alone |
| `release-packages` | test | linix | a `v*` tag or a `batch*` branch, where the package jobs run, after every package build it gathers and its install or test job | Every distribution release's `.deb` and `.rpm`, both AppImages and the source tarball, each held to its build's sums (the tarball to `build/check-source-tarball.py`) and given its SBOM, for `release` and the rehearsal (`.gitlab/ci/release-packages.yml`, `build/ci/release-chain.py gather`; [What a release carries](#the-release)) |
| `release-rehearsal`, `release-rehearsal-sign`, `release-rehearsal-verify` | release | GitLab's shared runners; the shared runners in cosign's image; linix | a protected `batch*` branch, where the package jobs run; never a tag, `master` or a merge request | The release rehearsed: collected, signed as the branch and verified, published nowhere (`.gitlab/ci/release-rehearsal.yml`; [The release rehearsal](#the-release-rehearsal)) |
| `helm-chart-package` | build | GitLab's shared runners | a `v*` tag or a `batch*` branch | The Helm chart packaged at the release's version as `hmailserver-chart-<version>.tgz`, a file of the release (`.gitlab/ci/helm-chart.yml`, `build/ci/helm-chart-package.sh`; [ContainerImage.md](ContainerImage.md)) |
| `container-image-deb` | release | GitLab's shared runners | a `v*` tag, started by hand after `release-publish`; refuses unless the project's variable `HM_PUBLISH_IMAGE` is `1` | The release's `.deb` and its SBOM taken from the published release and verified against their Sigstore bundles (`build/ci/container-image-deb.sh`; [The container image](#the-container-image)) |
| `container-image` | release | linix | a `v*` tag, after `container-image-deb`; refuses unless `HM_PUBLISH_IMAGE` is `1` | The release's container image built with kaniko from that `.deb`, inspected as `linux-image` inspects it, and pushed to the project's container registry (`build/ci/linux-image.sh` with `HM_IMAGE_PUSH=1`) |
| `container-image-sbom` | release | linix | a `v*` tag, after `container-image` | The image's SBOM: Syft over the pushed image by its digest, with the `.deb`'s own SBOM added (`build/ci/container-image-sbom.sh`) |
| `container-image-sign` | release | GitLab's shared runners | a `v*` tag, after `container-image-sbom` | The image signed by its digest with Sigstore as this pipeline, its SBOM attested to it, and both verified (`build/ci/container-image-sign.sh`) |
| `container_scanning` | release | GitLab's shared runners | a `v*` tag, after `container-image` | GitLab's own Container Scanning template over the pushed image; a finding does not fail the pipeline (`.gitlab/ci/container-image.yml`) |
| `container-image-start` | release | linix | a `v*` tag, after `container-image` | The pushed image started beside PostgreSQL and asked for SMTP and the REST API (`build/ci/container-image-start-test.sh`) |
| `pypi-publish` | release | GitLab's shared runners | `master`, started by hand | The release's `hmailserver-cli` to PyPI through a trusted publisher, with attestations; says "not configured" and passes until PyPI has the publisher, and fails once PyPI has the project and no publisher matches ([CommandLinePackages.md](CommandLinePackages.md)) |
| `psgallery-publish` | release | GitLab's shared runners | `master`, started by hand | The release's `HmailServer` module to the PowerShell Gallery; says "not configured" and passes until `PSGALLERY_API_KEY` is set, and refuses a module with no Authenticode signature, which the release does not make yet |
| `installer-smoke` | release | GitLab's shared runners | a `batch*` branch or a `v*` tag, started by hand | A placeholder. It fails with a sentence saying that the installer smoke test runs from the VMware host ([The installer smoke test](#the-installer-smoke-test)) |
| `renovate` | check | linix | the schedule described `renovate`, and nothing else | Renovate's dependency updates ([Dependency updates](#dependency-updates)) |
| `forge-export` | check | linix | the pipeline schedule for `master` whose description is `export`, and nothing else | The issues, merge requests, labels, milestones, boards and releases as JSON and the wiki as a git bundle, and GitLab's project export when its token is set: what a push mirror does not carry, kept on linix and as an artifact for Developers only (`.gitlab/ci/forge-export.yml`, `build/ci/forge-export.py`; [Mirroring.md](Mirroring.md)) |

`linux-build`, `windows-gate`, `windows-dotnet`, `linux-cross`, `linux-sanitize`
and `linux-sbom` start as soon as the pipeline does. They do not wait for the
checks, because the two machines are otherwise idle and the Windows gate is the
longest job. `linux-suite`, the two upgrade gates, the two package jobs,
`linux-image` and `linux-repro` wait for `linux-build`, whose artifact holds the
binary and the packages, and `release` waits for every job that can fail a tag's
pipeline - the checks, both suites, the upgrade gates, the package jobs,
`linux-cross`, `linux-sanitize`, `linux-image`, `linux-repro`, `windows-dotnet` -
and `windows-installer` ([The release](#the-release)). linix runs one
job at a time, so its jobs queue behind each other in the order GitLab hands them
out; bench150's two jobs take the bench's gate lock in turn
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
| `master` | the branch's | all of them but the release jobs, `linux-fuzz`, `installer-smoke` and `sign-off` |
| a `batch*` branch | the branch's | all of them but the release jobs, `linux-fuzz`, `pages`, `package-repos`, `semgrep-sast` and `secret_detection`, with `installer-smoke` manual: this is the gate a batch passes before it lands |
| any other branch, no open merge request | the branch's | `checkers`, `harnesses`, `editorconfig`, and the four path-bound checks whose files changed |
| any other branch, with an open merge request | the merge request's | `checkers`, `harnesses`, `editorconfig`, the path-bound checks whose files the merge request changes, `semgrep-sast`, `secret_detection`, and `sign-off` |
| a merge request from `master` or a `batch*` branch | none; the branch's own pipeline stands for it | as for the branch |
| a `v*` tag | the tag's | all of them but `linux-fuzz`, `semgrep-sast`, `secret_detection` and `sign-off` (the commit's `master` pipeline ran the first three, and its batch the last), with `release`, `release-publish` and `installer-smoke` manual ([The release](#the-release)) |
| any other tag | none | |
| the schedule described `fuzz`, on `master` | the schedule's | `linux-fuzz` alone ([The fuzz schedule](#the-fuzz-schedule)) |
| the schedule described `coverage`, on `master` | the schedule's | `linux-coverage` alone ([The coverage schedule](#the-coverage-schedule)) |
| the schedule described `renovate`, on `master` | the schedule's | `renovate` alone ([Dependency updates](#dependency-updates)) |
| the schedule described `security`, on `master`, run once by hand | the schedule's | `secret_detection` alone, over the whole history ([Security scanning](#security-scanning)) |
| the schedule described `export`, on `master` | the schedule's | `forge-export` alone ([Mirroring.md](Mirroring.md)) |
| any other pipeline schedule on `master` (the update feed's, `update feed`) | the schedule's | `pages` and `package-repos` alone (see [The update feed](#the-update-feed)) |
| the pipeline `update-feed` starts (on `master`, source `pipeline`) | a downstream pipeline | `pages` and `package-repos` alone |

A schedule is told apart by its description, which GitLab gives the pipeline
as `CI_PIPELINE_SCHEDULE_DESCRIPTION`, compared whole: `fuzz`, `coverage`,
`renovate`, `export` and `security` are the five words that start a job of their own,
which `pages` and `package-repos` refuse, and a schedule
carries no variable. The project refuses pipeline variables (*Minimum role to
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
machines.** Every job on linix and bench150 (`.own-runners`, and `db-matrix`
with it) runs only when the push changes a file that is not a Markdown file at
the root of the tree (`README.md`, `CHANGELOG.md`, `Roadmap2.md` and the rest)
or under `.gitlab/` (the issue and merge request templates). Every other
Markdown file counts as code: `hmailserver/docs/*.md` is compiled into the
server as its help pages (`HelpPagesData.cpp`) and read by both suites, and the
READMEs further down are read by tests and packaging. The shared runners' checks
still run for such a push. GitLab matches `rules:changes` paths with Ruby's
`File.fnmatch`, which has no "all but", so the rule names what does count:
`**/*[!d]`, `**/*[!m]d` and `**/*[!.]md` (every file whose name does not end in
`.md`) and `[!.]*/**/*.md` (a Markdown file below a directory whose name does not
begin with a dot). `build/check-release-chain.py` matches every file of the tree
against them and fails unless exactly the root's and `.gitlab/`'s Markdown files
are left out. A branch's first push, a tag and a pipeline started by hand count
every file as changed, so the gates always run for those.

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

Both of the project's runners are registered `ref_protected`, with `run_untagged`
off and `locked`. GitLab gives them a job only when all three of these hold:

1. the pipeline is for a protected ref: `master`, a `batch*` branch or a `v*`
   tag, which only the maintainers can push;
2. the job names one of the runner's tags (`linix`, `bench150`);
3. the job's `rules:` created it. `.own-runners` in the CI file creates its
   jobs only when `$CI_COMMIT_REF_PROTECTED` is `"true"` and the pipeline is not
   a merge request's, not the fuzz pipeline (which `linux-fuzz`'s own rule
   also holds to a protected ref) and not a scheduled one.

A merge request from a fork runs in the fork's context, on an unprotected ref,
with code nobody here has reviewed. The code could build anything, and both
machines hold things worth protecting: bench150's service runs as LocalSystem,
and linix has the lab network. So a fork's merge request gets the shared
runners' checks and the sign-off check, and nothing else. The protection is
kept in three places on purpose: the runners, the ref and the rules. Any one of
them is enough. Relaxing one of them is not a reason to relax the others.

The protected refs are project settings (Settings, Repository, Protected
branches and Protected tags): `master`, `batch*` and `v*`, with push and merge
allowed to Maintainers. `master`'s push rule is to become *No one* after the
first landing through a merge request ([Landing on master](#landing-on-master)).

Landing on master
-----------------

From the first batch it lands, a change reaches `master` through a merge
request. The script was written on 26 September 2026 and no batch has landed
through it yet; until one has, the owner pushes a gated batch to `master`
himself, as before. The owner's work is folded into a `batch*` branch, and
`build/land-via-mr.py` lands it:

```
python3 build/land-via-mr.py batch41 --dry-run
python3 build/land-via-mr.py batch41
```

It needs git and `glab`, logged in to gitlab.com as a Maintainer. In order, and
stopping at the first refusal:

1. **Before anything is sent**, the batch must be on top of the remote's
   `master` (the project merges by fast-forward only), add no merge commit, and
   carry its author's `Signed-off-by` on every commit it adds, less those the
   project's `v*` tags already hold (`build/sign-off.py`, the check the
   `sign-off` job runs); and the secret scan of every commit it adds
   (`build/scan-secrets.py`, [Before a push](#before-a-push-the-secret-scan))
   must run and find nothing: a possible secret, or no scanner to run it,
   refuses (exit 8). A `batch*` branch's pipeline does not run
   `secret_detection`, and its merge request runs no pipeline, so this is the
   scan that reads a batch before it is on GitLab. `--dry-run` stops here.
2. It pushes the branch. A `batch*` branch is protected, so its pipeline runs
   everything, the two machines' gates included.
3. It opens a merge request into `master`, not squashed, with the source branch
   to be deleted, or finds the one already open for the branch: running it again
   resumes. `--no-wait` stops here.
4. It waits for the pipeline of exactly the branch's tip (twelve hours by
   default, `--timeout-hours`). A merge request from a `batch*` branch runs no
   pipeline of its own (the `workflow:` rules), so GitLab holds it to the
   branch's pipeline for the same commit, its head pipeline. Anything but
   success refuses: failed, canceled, skipped, or waiting on a manual job that
   blocks.
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
   `squash` false - the project's squash option is *encouraged*, and a squash
   would put one commit in the merge request author's name in place of the
   batch, its authors and its sign-offs - and checks that the remote's `master`
   is now that tip.
8. It records the landing in a note on the merge request - the tip, the
   pipeline, the sign-off, the approvals and who landed it - and in
   `land-via-mr.log` in the repository's git directory. It moves the local
   `master` to the tip when no worktree has it checked out, and deletes the
   branch on GitLab and locally where it can, saying when it cannot.

Nothing reaches `master` when it refuses. After a failed pipeline or an open
thread, change the batch, push it again, and run it again; after `master` has
moved, rebase the batch onto it first, which starts a new pipeline.

Merging by fast-forward is a push to `master`, so `master`'s own pipeline runs
the whole gate again on the same commit ([Which pipeline runs
what](#which-pipeline-runs-what)); the `sign-off` job does not run there, since
every commit a merge request brings to `master` has passed it on the way. A
direct push to `master`, which Maintainers can make until its push rule is *No
one* (below), is checked by nothing.

**What has run, and what has not.** `python3 build/land-via-mr.py --self-test`,
which `checkers` runs, drives the whole landing against a bare repository
standing for the project and a GitLab API held in memory: a landing, and a
refusal for each of an unsigned commit, a possible secret, a secret scan that
could not run or failed (the scan itself stands in there; its own self-test
covers it), a batch behind `master`, a merge commit,
a failed pipeline, a pipeline waiting on a manual job, an open thread, and a
missing approval once one is required; approvals read from the merge request's
system notes where the approvals API is refused; an approval given before the
tip reached the merge request not counted, and counted with a note where the
diff versions cannot be read; and the release-tag exemption
taken from the project's tags, never from a tag only the local clone has. No
batch has landed this way on GitLab yet. The first landing is the first test of
the calls against GitLab itself: the merge request, its head pipeline, the
approvals and diff-versions endpoints on the Free tier (their answers' shapes
are taken from GitLab's API documentation; the project has had no merge request
to read them from), the merge, and deleting a protected branch.

### After the first landing: nobody pushes to master

`master`'s rule lets Maintainers push and merge. Once a batch has landed through
`build/land-via-mr.py` - its branch's pipeline green and the merge made through
the API - nothing needs a direct push any more, and the rule drops it for
everyone: **push *No one*, merge *Maintainers***. Not before: until the merge
request path has worked once on GitLab, a direct push is the way a fix lands.

The owner makes the change in Settings, Repository, Protected branches: for
`master`, *Allowed to push and merge* becomes *No one*, and *Allowed to merge*
stays *Maintainers*. The edit is in place. Through the API the same takes two
calls on the Free tier, whose `PATCH` of a protected branch cannot change an
access level (`allowed_to_push` is Premium's), so the branch is unprotected and
protected again, and is unprotected for the moment between the two:

```
glab api -X DELETE projects/86730559/protected_branches/master && \
glab api -X POST projects/86730559/protected_branches -f name=master \
  -F push_access_level=0 -F merge_access_level=40 -F unprotect_access_level=40 \
  -F allow_force_push=false
glab api projects/86730559/protected_branches/master
```

The last must show `push_access_levels` holding one entry, `access_level` 0
(*No access*), `merge_access_levels` Maintainers, and `allow_force_push` false.
Nothing then pushes to `master`, the owner included: no hand's `git push`, and
no job (Renovate pushes `renovate/*` branches and opens
merge requests; `pages` and `forge-export` push nothing). Tags are not touched:
the release tag is still the owner's to push. To undo it, set *Allowed to push
and merge* back to *Maintainers*.

The runners
-----------

### linix

The Ubuntu 26.04 lab VM at 192.168.11.154: 8 cores, 11 GB of memory, 200 GB of
disk. GitLab Runner 19.4 runs there as a system service, with the docker
executor, `concurrent = 1`, and six CPUs and 8 GB of memory for each job. The
default image is `ubuntu:26.04`, the pull policy is `if-not-present`, and the
runner is not privileged. It has the tags `linux`, `docker` and `linix`, and it
is runner 56743206 on GitLab.

What the host needs:

- Docker (29.1 today) and the runner.
- The job image, `hmailserver-ci/linux:<tag>`, built on the host (see
  [The image](#the-image)).
- `postgres:16` at the digest the CI file names. It is pulled on first use, and
  it is on the host already.
- The other images the ported jobs name, each by digest, each pulled on first
  use: `mariadb:11` (the MySQL upgrade gate's service) and `ubuntu:26.04` from
  Docker Hub, `registry.fedoraproject.org/fedora:44`, and
  `gcr.io/kaniko-project/executor:v1.24.0-debug`. If the runner's `config.toml`
  restricts images (`allowed_images`) or registries, these must be allowed.
- The runner's local cache, which carries the fuzz corpus from night to night
  (`cache:` in `linux-fuzz`). The docker executor keeps it in a volume on the
  host; nothing needs setting up.

Nothing else is installed on the host for CI. The toolchains, the .NET SDK,
PowerShell, Syft and the database clients are in the image.

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
  and on the machine PATH. LocalSystem sees the machine PATH only, and a
  PowerShell installed from the Microsoft Store is on no PATH but its
  installing user's. A service reads the machine PATH when it starts, so the
  runner service has to be restarted after PowerShell 7 is installed, and a
  PATH change can need a reboot before a service sees it.
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

The Linux jobs that build or run the server - `linux-build`, `linux-suite`,
`linux-cross`, `linux-sanitize`, the two upgrade gates, `linux-sbom`,
`linux-repro` and `linux-fuzz` - run in one image, described by `build/ci/linux.Dockerfile`. The
package jobs run in the distributions' own images instead, because the point
is a clean system, and `linux-image` in kaniko's. The image holds:

- Ubuntu 26.04 at a pinned digest;
- the toolchain and the `-dev` packages of the Linux build;
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

The image is built **on linix and never pushed anywhere**. Jobs find it in the
host's image store under its tag, and the `if-not-present` pull policy means
they never try to pull it.

The tag is made from the Dockerfile: `hmailserver-ci/linux:26.04-<hash>`, where
`<hash>` is the first twelve hex digits of the Dockerfile's SHA-256 with its
line endings made LF. `build/check-ci-image.py`, one of the checkers, fails
until the CI file names the tag of the Dockerfile as it is. So an edit to the
Dockerfile goes like this:

1. Edit `build/ci/linux.Dockerfile`, and put the new tag that the checker
   prints into `.gitlab-ci.yml`, in the same commit.
2. Before the first pipeline for that commit reaches linix, build the image on
   linix from a checkout of the commit, as a member of the `docker` group:

       bash build/ci/build-linux-image.sh

   The script prints the tag it built. It removes no older tag, because a
   pipeline for an older commit still asks for its own tag. It lists them
   instead, and `docker image rm <tag>` removes one that is no longer wanted.
3. If the image was not built, the Linux jobs fail at their first step, naming
   the image they could not find. That is by design. The alternative is a job
   that runs on an image made from an older Dockerfile and says nothing.

**Why the image is built by hand and not by a job.** A job that builds an image
needs the host's Docker socket mounted into the runner's containers. That would
give every job on linix root on linix: every job for a protected ref, and every
job any future change to the rules lets through. The integrator decides on
socket access. Until that decision is taken, the image is built by hand, and
the tag rule above makes sure no job runs on an image the Dockerfile no longer
describes. (`linux-image` builds the product's image without a socket, with
kaniko, but kaniko writes a tarball or pushes to a registry: it cannot put an
image into linix's own image store, which is where the jobs look for theirs.)

The Windows gate
----------------

`windows-gate` works in `C:\bench\hmailserver`, not in the runner's builds
directory, because the bench's hMailServer service is registered from that
path. The bench keeps its registration. The job has `GIT_STRATEGY: none`, so the
runner clones nothing, and the job does the following itself:

1. It fetches the commit into the bench's checkout from
   `https://gitlab.com/Progressiverobot/hmailserver.git` (`HM_GATE_REPOSITORY`).
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
   5. It starts clamd if it is down, and runs `preflight-tests.ps1 -Clean`
      (exit 2).
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
| `linux-build.yml` build, x86-64 GCC | not ported: see below | | |
| `linux-build.yml` build, AArch64 | not ported: see below | | |
| `linux-build.yml` build, the AppImage | the `appimage-*` jobs of `.gitlab/ci/packages-appimage.yml`, x86_64 and aarch64, with a runtime of the project's own and no linuxdeploy ([PackagesAppImage.md](PackagesAppImage.md)); CI job artifacts, not in a release | linix | a `v*` tag; by hand on `master` or a `batch*` branch |
| `linux-build.yml` cross | `linux-cross` | linix | a protected ref |
| `linux-build.yml` package-install | `linux-package-deb`, `linux-package-rpm` | linix | a protected ref |
| `linux-build.yml` regression-on-linux | `linux-suite` (row 139), the whole suite in one run | linix | a protected ref |
| `linux-build.yml` db-upgrade | `linux-db-upgrade-pgsql`, `linux-db-upgrade-mysql` | linix | a protected ref |
| `linux-build.yml` image | `linux-image`: built and inspected, not pushed; for a `v*` tag, `container-image` pushes the release's image and `container-image-sign` signs it | linix, shared | a protected ref; a `v*` tag |
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
| `dependency-review.yml` | not ported: see below | | |
| `scorecard.yml` | not ported: see below | | |
| `upstream-watch.yml` | not ported: see below | | |
| `sign-release.yml` | `release-authenticode`, `release-sign` and `release-verify`: Authenticode with a federated credential in place of a client secret, and Sigstore; not the SLSA provenance (see below) | bench150, shared, linix | a `v*` tag |
| `verify-binary-provenance.yml` | not ported: the release-chain row (137) | | |
| `installer-smoke.yml` | `build/ci/installer-smoke-run.sh` from the VMware host, and a placeholder `installer-smoke` job ([The installer smoke test](#the-installer-smoke-test)) | the VMware host, not a runner | by hand, before a release is published |

**Not ported, and why:**

- `codeql.yml`. The CodeQL CLI and its query packs are GitHub's, downloaded
  from GitHub, and its results went to GitHub's code scanning. There is no
  other source for either.
- `dependency-review.yml` and `scorecard.yml`. Both are GitHub services: the
  first reads GitHub's advisory database for a pull request's dependency
  changes, the second scores the repository through GitHub's API and publishes
  to the OpenSSF API from a verified GitHub workflow.
- `upstream-watch.yml`. It reads upstream hMailServer's repository, which is on
  GitHub, and files an issue there.
- `verify-binary-provenance.yml`, `package-managers.yml` manifests, and the SLSA provenance and the `.sigstore.json` copies of the
  bundles that `sign-release.yml` made. The SLSA generator is a GitHub reusable
  workflow with no GitLab form, and the copies were for OpenSSF Scorecard, which
  reads GitHub. They stay with the Roadmap2 row "The release chain off a single
  forge" (137); the signing and publishing it built are
  [The release](#the-release).
- `ci.yml`'s coverage upload. It went to GitHub's code-quality page. The tests
  still run; the coverage is not collected.
- The GCC build and the native AArch64 build of `linux-build.yml`'s matrix. linix
  is one x86-64 machine, and a second whole build per pipeline would add
  twelve minutes to its queue; `linux-cross` compiles every file for AArch64,
  and `bash build/linux-tu-census.sh --cc g++` is the GCC census by hand.

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
| `linux-build`, `linux-suite`, `linux-cross`, `linux-sanitize`, `linux-db-upgrade-*`, `linux-sbom`, `linux-repro`, `linux-fuzz` | `hmailserver-ci/linux` (built on linix) | Ubuntu's packages; the .NET SDK and PowerShell from Microsoft's SDK image; Syft 1.52.0 from `anchore/syft` on Docker Hub ([The image](#the-image)) |
| `linux-db-upgrade-*` services | `postgres:16`, `mariadb:11` (Docker Hub) | the database servers |
| `linux-package-deb` | `ubuntu:26.04` | the package's dependencies, PostgreSQL's client, curl and iproute2 from Ubuntu's archive |
| `linux-package-rpm` | `registry.fedoraproject.org/fedora:44` (Fedora) | `rpm`, in the image |
| `linux-image` | `gcr.io/kaniko-project/executor:v1.24.0-debug` (Google) | kaniko, and its busybox; the image it builds installs the `.deb` from Ubuntu's archive |
| `windows-dotnet` | bench150's own | the .NET SDK, Visual Studio's Build Tools and the .NET Framework SDK, from Microsoft; NuGet packages from nuget.org, held to the lock files |

The .NET job
------------

`windows-dotnet` is `ci.yml`'s two jobs and `style.yml`'s `dotnet format` in
one, on bench150, run by `build/ci/windows-dotnet.ps1`: the settings search
index is what `generate-settings-index.ps1` writes; the COM wrapper made from
the IDL alone (`generate-com-wrapper.ps1 -FromIdl`); the Control Panel restored
against its lock file, built with warnings as errors, published and tested; the
tools' solution likewise, the Import Tool's tests, and `build-tools.ps1`'s
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
(`linux-build/packages/`, with their SHA-256 sums). A packaging failure does
not fail `linux-build` - the suite needs the binary, not the packages - but the
package jobs, `linux-image` and `linux-repro` fail on the missing files and point
at `linux-build/package.log`.

`linux-package-deb` installs the `.deb` in a clean Ubuntu 26.04 container with
`apt`, asserts every file a working installation needs, configures the packaged
INI against a PostgreSQL service the way the packaging README tells an
administrator to, and runs the server's own verbs as the service user. Then it
starts the server as its unit would - as `hmailserver`, in `/var/lib/hmailserver`,
with `CAP_NET_BIND_SERVICE` alone and no new privileges, through `setpriv` - and
asks whether the REST API answers and whether `GET /` is the Control Deck, byte
for byte. What a container cannot show is the start under systemd itself, with
the unit's sandbox in force: the old job's hosted runner had systemd, and a job
container does not. `update-helper` still verifies the units.
`linux-package-rpm` installs the `.rpm` in Fedora 44 with `--nodeps`, as before:
it is built on Ubuntu, so its requirements name Ubuntu's sonames.

The package is built on Ubuntu 26.04 now, where the GitHub runner built it on
24.04; its `Depends` asks for glibc 2.38 or later, which 24.04 has, and the
container image, whose base is still Ubuntu 24.04, installs it.

`linux-image` builds `platform/packaging/docker/Dockerfile` from that `.deb`
with kaniko, which needs no Docker daemon and no privilege, into a tarball, and
checks what it built: the user, the entrypoint, the healthcheck, the nine
ports, and the file capability `setcap` gave `/usr/bin/hmailserver`, read from
the layer. It does not run the image; that needs a daemon. The tarball is not
kept: for a `v*` tag, `container-image` builds the image again from the `.deb`
the release carries and pushes it ([The container image](#the-container-image)).

The fuzz schedule
-----------------

`linux-fuzz` is the old nightly fuzz workflow, moved from a hosted Windows runner
with clang-cl to linix with clang: the same three MIME harnesses under `fuzz/`,
the same dictionary, the same reproducers and the same libFuzzer flags, built by
`build/ci/linux-fuzz.sh` (`fuzz/build-fuzz.ps1` stays the way to build them on
Windows). It replays every kept input and every committed reproducer first, then
fuzzes each harness for `HM_FUZZ_MINUTES` (20) with `HM_FUZZ_JOBS` workers (2),
on the corpus the earlier nights grew, which the runner's cache carries from one
night to the next. A finding fails the job. The job's artifact is
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
pipeline started that way is not a schedule's. It occupies linix for a little
over an hour, and a protected ref's pipeline that arrives meanwhile waits for
it. To start the corpus again from the seeds, clear the runner caches
(*Pipelines*, *Clear runner caches*).

The schedule was made on 27 September 2026: schedule 4461177, `fuzz`,
`10 2 * * *` in UTC, on `master`, active. By 00:40 UTC on 28 September it had
run no pipeline; its first was due at 02:10 that day.

The coverage schedule
---------------------

`linux-coverage` measures how much of the native server the tests reach, the
figure the OpenSSF criteria `test_statement_coverage80`, `test_statement_coverage90`
and `test_branch_coverage80` ask for. `build/ci/linux-coverage.sh` configures the
server with `-DHM_COVERAGE=ON` (clang's `-fprofile-instr-generate
-fcoverage-mapping`, counters written continuously so that a server stopped by
SIGKILL still leaves them), runs the `*-tests` programs, then
`build/ci/linux-suite.sh` - the whole of LinuxRegressionTests on the same
PostgreSQL 16 service - against the instrumented server, and merges the
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

- `update-helper`: GitLab's shared runners check out with umask 0000, and
  `systemd-analyze verify` refused the world-writable units. The job gives
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
   process to exit. The stop gets `ShutdownDrainSeconds` plus 30 seconds, and
   the process gets 30 seconds more. The upgrade's stop at the Ready page does
   the same. Every file the server's process loads is marked
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
written on 26 September 2026, while 6.3.4 was released by hand, and **none of it
has run**: see [What waits for the owner](#what-waits-for-the-owner) and
[What only a first run can show](#what-only-a-first-run-can-show). RELEASE.md
has the runbook.

| Job | Where | What it does |
|---|---|---|
| `windows-installer` | bench150, in `C:\bench\hmailserver` under the gate lock, after `windows-gate` | Refuses a checkout with a change of its own to a tracked file, with a file git neither tracks nor ignores under `hmailserver\source` (an SDK-style project builds every source file below its folder, tracked or not), or with an untracked `Directory.Build.props`, `NuGet.Config` or kin that every build would read, in the checkout or above it; empties every folder the installer script takes whole from the checkout, and every tool's publish folder, of what git does not track; then builds the server, the tools and the Control Panel, fetches the .NET runtime and the third-party files (each against its pinned hash), refuses if a build changed a tracked file, runs ISCC, and requires the file to be `hMailServer-<version>-x64.exe` with the version resource `<version>.0` (`build/ci/windows-installer.ps1`). Automatic: it signs and sends nothing |
| `release` | shared runners, by hand, after every gate | Refuses unless the tag is an annotated tag that still points at the commit the pipeline built, that commit is on `master`, and the tag is signed by the one key `master`'s `.gitlab/allowed_signers` lists (fetched by name, never the tagged tree's copy; every tag is refused while that list holds more than one key), `Version.h` says the tag's version, the security scans of the commit finished and every finding they raised that `SECURITY.md`'s policy counts is in a baseline or accepted in `.gitlab/security-exceptions.yml` ([Before a release](#before-a-release-the-scans-hold-it)), `HM_COSIGN_IMAGE` is `gcr.io/projectsigstore/cosign` pinned by digest (checked as the job receives it, so a CI/CD variable of that name cannot put another image in `release-sign`) and the five Azure settings are set; then collects the installer; the x86-64 `.deb` and `.rpm`, `linux-build`'s, and the generic AArch64 ones, `deb-ubuntu26.04-arm64`'s and `rpm-fedora44-aarch64`'s (from 6.3.6), each with its own SBOM (`<package>.cdx.json`, which `linux-build` or the package's SBOM job made and which must describe the package, by SHA-256); from 6.3.6 every distribution release's `.deb` and `.rpm`, both AppImages and the source tarball, each with its SBOM, as `release-packages` gathered them, and the Helm chart, `helm-chart-package`'s; the sums as `hmailserver-linux-<version>-SHA256SUMS.txt`, `linux-build`'s `SHA256SUMS.txt` as it is with a line added for every other Linux file and the chart; and the tree's two SBOMs, checking every package, AppImage and the chart against the sums its build job wrote and the tarball with `build/check-source-tarball.py`; refuses when the package `hmailserver/<version>` already holds one of those files with other bytes, or the installer, its SBOM, a bundle, a bundle's `.sigstore.json` copy or the provenance at all, which only the jobs after it make; and last counts what GitLab and Pages will take of the release, refusing past a limit ([What a release carries](#the-release); `build/ci/release-chain.py collect`) |
| `release-authenticode` | bench150 | Signs the installer through Azure Artifact Signing and requires a valid signature with an RFC 3161 timestamp (`build/ci/windows-authenticode.ps1`) |
| `release-sbom` | shared runners, in Python's image | Makes the installer's own SBOM, `hMailServer-<version>-x64.exe.cdx.json`, from the bytes Authenticode signed, with `linux-sbom`'s component list (kept by `release` in `release-tools/`), the tools' lock files and the installer script; refuses an installer Authenticode left unchanged; then writes the statement of the release's SLSA v1 provenance, an in-toto statement naming all fifty-six files by their SHA-256, the commit and the pipeline, for `release-sign` (`build/ci/release-chain.py sbom`, `build/asset-sbom.py`) |
| `release-sign` | shared runners, in cosign's `gcr.io` image by digest | Requires the installer's SBOM and the provenance's statement to name the signed installer's SHA-256, then signs all fifty-six files with Sigstore, checks each bundle's format and log entry and verifies it with cosign, copies each primary file's bundle to `<file>.sigstore.json` byte for byte, attests the statement with `cosign attest-blob` (a Rekor v1 dsse entry), and finds every file in it with `cosign verify-blob-attestation` (`build/ci/release-sign.sh`) |
| `release-verify` | linix | Checks every file with `hmailserver --verify-update`, built by `linux-build` from the same commit, requires the GitLab identity, refuses a copy with a byte added, holds each compiled file's and the tarball's SBOM to the file (`build/asset-sbom.py verify`), each `.sigstore.json` copy to its bundle and the provenance's statement to the files, the commit and the pipeline (`build/ci/release-chain.py check-provenance`), and writes the notes' verification section (`build/ci/release-verify.sh`) |
| `release-publish` | shared runners, by hand | Refuses a release that exists already, a compiled file whose SBOM is missing or describes other bytes, a `.sigstore.json` that is not its bundle, a provenance that does not name every file, a link GitLab would refuse, a site Pages or a server would not read whole, and, before it sends anything, a file name the generic package `hmailserver/<version>` holds with other bytes; uploads the 143 files there, except any it holds with the same bytes from a run that stopped part-way, reads them back and holds every record of each name to the SHA-256 that was signed, creates the release (the version's section of `CHANGELOG.md` and the verification) with a link per file named after it, and requires the feed rendered from the API to give Windows servers, Debian and RPM servers on x86-64 and AArch64, and the AppImages their file and bundle (`build/ci/release-chain.py publish`) |
| `update-feed` | a trigger | A downstream pipeline on `master`, which runs `pages` and `package-repos` alone: every other job refuses a pipeline whose source is `pipeline`. It passes no variables, not even the file's (`inherit: variables: false`), because GitLab takes a trigger job's variables as pipeline variables, which this project refuses at GitLab's default for a project made since 17.7 |

**Why two buttons.** Nothing is signed or sent until a maintainer starts
`release`, after every gate of the tag has passed. A manual job can be started
as soon as its own needs have finished, so `release` needs every job of
`.gitlab-ci.yml` that can fail the tag's pipeline, and waits for the four
security scans whose reports it reads (`static-analysis`, `semgrep`,
`dependency-scan` and `semgrep-sast`), which are `allow_failure`: it reads
their reports and refuses on what they found
([Before a release](#before-a-release-the-scans-hold-it)). Of the package
jobs of `.gitlab/ci/packages-*.yml`, which a tag's pipeline runs too, it needs
the two whose generic files the release carries, `deb-ubuntu26.04-arm64` and
`rpm-fedora44-aarch64`, their SBOM jobs and their install jobs, which install
and run each package in an AArch64 container of its distribution under
qemu-user and so are gates; `release-packages`, which needs every other package
build and its install job, both AppImages and their test jobs, and the source
tarball; and `helm-chart-package`. It brings the artifacts of thirteen jobs: the
nine whose files it collects (`linux-build`, `linux-sbom`, `windows-installer`,
the two package builds and their SBOM jobs, `release-packages` and
`helm-chart-package`) and the four scans. `release-packages` is a job of its own
because GitLab.com lets a job need fifty jobs at most (`ci_needs_size_limit`):
`release` needs thirty-three, and `release-packages` forty-one, thirty-nine of
them jobs `release` does not need itself.
It does not wait for `ci-image-packages` and `repo-hygiene`, which are
`allow_failure`, for the manual `db-matrix` and `installer-smoke`, or for the
Windows packages, whose installers no release carries: they are not gates of a
release, and `release` can start while they are still running or after one has
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

**What a release carries.** From 6.3.6, fifty-six files, a bundle beside each,
a `.sigstore.json` copy of each primary file's bundle, and the SLSA provenance:
143 files and as many links (`build/ci/release-chain.py`'s `release_files`).

| Files | How many | Made by |
|---|---|---|
| The Windows installer, `hMailServer-<v>-x64.exe` | 1 | `windows-installer`, signed by `release-authenticode` |
| The generic `.deb` and `.rpm` for x86-64 (`hmailserver_<v>_amd64.deb`, `hmailserver-<v>-1.x86_64.rpm`) and AArch64 (`hmailserver_<v>_arm64.deb`, `hmailserver-<v>-1.aarch64.rpm`), the names an installed server's update check asks for | 4 | `linux-build`; `deb-ubuntu26.04-arm64`, cross-compiled in Ubuntu 26.04's own container; `rpm-fedora44-aarch64`, in Fedora 44's |
| Each distribution release's own package, both architectures: `hmailserver_<v>-1~<release>_<arch>.deb` for Debian 12 and 13 and Ubuntu 24.04, `hmailserver-<v>-1.<release>.<arch>.rpm` for Fedora 44 and 43, EL 10 and 9, openSUSE Leap 16.0 and Tumbleweed | 18 | `deb-<release>-<arch>` and `rpm-<release>-<arch>` (`.gitlab/ci/packages-deb.yml`, `packages-rpm.yml`), each in its release's own container |
| The AppImages, `hMailServer-<v>-x86_64.AppImage` and `-aarch64.AppImage`, with the project's own runtime | 2 | `appimage-x86_64`, `appimage-aarch64` (`.gitlab/ci/packages-appimage.yml`) |
| The source tarball, `hmailserver-<v>.tar.gz`, with nothing non-free in it | 1 | `source-tarball` ([SourceTarball.md](SourceTarball.md)) |
| The Helm chart, `hmailserver-chart-<v>.tgz` | 1 | `helm-chart-package` ([ContainerImage.md](ContainerImage.md)) |
| `hmailserver-linux-<v>-SHA256SUMS.txt`: `linux-build`'s two lines, then one for every other Linux file and the chart | 1 | `release` |
| Each compiled file's and the tarball's own SBOM, `<file>.cdx.json` (CycloneDX 1.6; SECURITY.md, "Software bills of materials") | 26 | `linux-build`, the two generic AArch64 packages' SBOM jobs, `release-packages`, and `release-sbom` for the installer |
| The SPDX and CycloneDX SBOMs of the tree | 2 | `linux-sbom` |
| `<file>.cosign.bundle` beside each of those 56; `<file>.sigstore.json`, a byte-identical copy, beside each of the 30 but a compiled file's SBOM | 86 | `release-sign` |
| `hmailserver-<v>.intoto.sigstore.json`, the SLSA v1 build provenance: an in-toto statement naming all 56 by their SHA-256, the commit and the tag's pipeline, signed with `cosign attest-blob` | 1 | `release-sbom` writes the statement, `release-sign` attests it |

6.3.5 carries nine files: the installer, the x86-64 `.deb` and `.rpm`, the sums,
their SBOMs and the tree's, each with a bundle. The AArch64 packages, each
distribution release's AArch64 package and the aarch64 AppImage are tested under
emulation only: their install and test jobs run each in an AArch64 container
under qemu-user on linix, and the project has no AArch64 machine. A package
job's container has neither PowerShell nor Python, so the build records what it
was built against (`built-against/`: what dpkg or the sysroot's rpm database
held, and Boost's header; an AppImage's build also what it bundles) and the SBOM
is made from that in the CI image (`build/ci/packages/package-sbom.sh`), by the
generic AArch64 packages' SBOM jobs and by `release-packages`, which gathers the
rest with the tarball (`build/ci/release-chain.py gather`) and holds each to the
sums its build wrote. The ARM64 installer is not collected: `windows-packages-arm64`
builds it once bench150 has what it needs, and it joins the release, with its
SBOM and its own Authenticode signature, only once it has run on ARM64 hardware.
The provenance is the project's own, verified with cosign (SECURITY.md, "Build
provenance"): the SLSA project's generator and `slsa-verifier` are GitHub's. The
verification section `release-verify` writes into a release's notes says all of
this. The server and the tools in the installer are the bench checkout's
incremental build of the tag's commit, not a clean one, and its OpenSSL, libpq
and C++ runtime come from the bench's library tree and Visual Studio; the notes
say that too.

**What GitLab and Pages take.** GitLab sets no limit on a release's links, but
each link's permanent path must be at most 128 characters of letters, digits and
`_ . - /` (6.3.6's longest is 76; the `~` of a `.deb`'s version, which GitLab
refuses there, is written `-`), and each link's name, address and path its own.
Pages reads 1,000 `_redirects` rules and 64 KiB, and a server reads a feed of
4 MiB. `release` counts all of it before anything is signed, and
`release-publish` again before anything is uploaded, over the live releases with
the new one in them and the apt and dnf repositories kept as `package-repos`'
`--keep` keeps them, and refuses past a limit (`check_budget`). 6.3.6 alone: 143
links; `_redirects` 31 rules in 5,386 bytes, the feed's 15 and the apt and dnf
repositories' 16; `releases.json` 48,524 bytes. Each later release of its shape
adds 16 rules, 2,942 bytes and 48,521 bytes, so twenty-one such releases fit in
`_redirects` before `--keep` is needed, and some eighty-five in the feed
([UpdateFeed.md](UpdateFeed.md), "How much the site holds";
[PackageManagers.md](PackageManagers.md), "What the repositories hold").

### The release rehearsal

A protected `batch*` branch's pipeline builds and tests everything a tag's does,
so it also rehearses the release, publishing nothing
(`.gitlab/ci/release-rehearsal.yml`):

- `release-rehearsal`, on the shared runners, runs `build/ci/release-chain.py
  collect --rehearse`: every file the release carries, gathered under the names
  it carries them, each held to its build's sums and its SBOM as `collect` holds
  them for a tag, and the budget above counted for this release alone. The
  version is `Version.h`'s. The x64 installer is `windows-packages-x64`'s,
  `hMailServer-<v>-<first 8 of the commit>-x64.exe`, held to that job's
  `SHA256SUMS` and collected under the release's name, because
  `windows-installer` runs for a `v*` tag alone. Then `sbom --rehearse` makes
  the installer's SBOM and the provenance's statement.
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
chart pull it from there; 6.3.4 was built by hand and has none. The jobs of
`.gitlab/ci/container-image.yml` are in every protected `v*` tag's pipeline,
after `release-publish`: a maintainer starts the first, `container-image-deb`,
by hand, and the rest follow it. They push only while the project's CI/CD
variable `HM_PUBLISH_IMAGE` is `1`, which the scripts read as the jobs run -
not a rule, which is decided when the pipeline is made - so a release
published before the project's move into the group (item 3 below) has its
image pushed from its own pipeline after it ([ContainerImage.md](ContainerImage.md),
"Publishing"):

| Job | Where | What it does |
|---|---|---|
| `container-image-deb` | GitLab's shared runners, in cosign's image (`HM_COSIGN_IMAGE`); started by hand, and the tag's pipeline shows as blocked until it is | Refuses, exit 3, unless `HM_PUBLISH_IMAGE` is `1`. Takes `hmailserver_<version>_amd64.deb` and its SBOM from the published release - not from `release-sign`'s artifact, which may have expired by then - and verifies each against its Sigstore bundle as this project's pipeline for the tag, at either of the project's two paths, with the project's id required at the group's (`build/ci/container-image-deb.sh`) |
| `container-image` | linix, in kaniko's image (`linux-image`'s, by reference) | Refuses, exit 3, unless `HM_PUBLISH_IMAGE` is `1`. Builds `platform/packaging/docker/Dockerfile` from that `.deb` and pushes it to `$CI_REGISTRY_IMAGE`, which must be the group's path, `registry.gitlab.com/hmailserver/hmailserver` (at the project's first path it refuses, exit 3, and pushes nothing), with the job's own `CI_REGISTRY_USER` and `CI_REGISTRY_PASSWORD`. The tags are `:<version>`, and for a version without a suffix `:<major>.<minor>` and `:latest` too: `v6.3.5` pushes `6.3.5`, `6.3` and `latest`, and `v6.3.5-rc1` pushes `6.3.5-rc1` alone. It runs the same checks on what it pushed as `linux-image` does on what it built, and leaves a dotenv report naming the image by digest (`build/ci/linux-image.sh`, `HM_IMAGE_PUSH=1`) |
| `container-image-sbom` | linix, in the Linux CI image | Syft reads the pushed image from the registry by its digest, with the job's own registry credentials, and `build/asset-sbom.py image` adds what the release's `.deb` SBOM names inside the binary and makes the image, by its digest, the document's subject; it refuses a `.deb` SBOM that does not describe the `.deb`, and an image in which Syft finds no `hmailserver` package (`build/ci/container-image-sbom.sh`) |
| `container-image-sign` | GitLab's shared runners, in cosign's image (`HM_COSIGN_IMAGE`) | Signs the image by its digest with Sigstore, keylessly, as the same pipeline identity the release's files carry, and verifies the signature with that identity. The tags all name the one digest, so one signature covers them. The signature is stored beside the image in cosign's tag form, `sha256-<digest>.sig`, with Rekor v1 as the log. It then attests the image's SBOM to it (`cosign attest --type cyclonedx`, stored as `sha256-<digest>.att`) and verifies the attestation with the same identity (`build/ci/container-image-sign.sh`) |
| `container_scanning` | GitLab's shared runners | GitLab's Container Scanning template, which the Free tier runs, over the pushed image by digest, with the analyzer pinned by digest. It is `allow_failure`, as the template makes it; its report is the job's artifact, for members with the Developer role or above, because the vulnerability report is GitLab Ultimate |
| `container-image-start` | linix, in the Linux CI image | Starts the pushed image by its digest as a service beside PostgreSQL, on a network of the job's own, through its entrypoint, and asks it for SMTP on 25 (with STARTTLS) and for the REST API over TLS with the release's version (`build/ci/container-image-start-test.sh`, which also runs by hand against any image with `--docker`) |

**Why after `release-publish`.** `:latest` is what the Compose file and the chart
pull. A release the installer smoke test stopped before `release-publish` must
not be what `:latest` names, so the image waits for the release to exist, and is
built from the release's published `.deb`.

The Helm chart is a file of the release, made by `helm-chart-package`
(`.gitlab/ci/helm-chart.yml`) and served from the Pages site's `helm/`, which
`build/ci/helm-repo.py` writes from the feed ([ContainerImage.md](ContainerImage.md)).

**Checked after the push, not before.** kaniko cannot push a tarball it wrote
before, so `container-image` pushes and writes the tarball of the same image in
one run, and then checks it. A check that fails fails the job with the tags
already pushed: nothing signs or scans the image, and the job names the tags,
which then name an image that failed its check and are deleted by hand. The same checks passed on the same Dockerfile and
`.deb` in `linux-image` earlier in the pipeline; only the Ubuntu packages the
Dockerfile installs can differ between the two builds.

**To verify an image**, with cosign 2.4 or later:

```
cosign verify \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  registry.gitlab.com/hmailserver/hmailserver:<version>
```

The identity and the image are the group's because the image is pushed only
after the project's move (item 3 below); a release signed from the present
path, `https://gitlab.com/Progressiverobot/hmailserver//...`, 6.3.5 among them,
has no image. `container-image-sign` keeps
`container-image-signed/verification.txt`, with the command for that release.

**What the owner sets on the registry.** Nothing in the pipeline can. As the
project's API answered on 28 September 2026:

1. **The cleanup policy** is switched off. Its tags to keep
   (`name_regex_keep`) are five patterns: `^v?[0-9]+\.[0-9]+\.[0-9]+.*$`,
   `^latest$`, `^stable$`, `^[0-9]+\.[0-9]+$` (the `6.3` line) and
   `^sha256-[0-9a-f]{64}\.(sig|att|sbom)$` (cosign's signatures and
   attestations), so that, switched on, it never deletes a release's tags or
   their signatures; it would remove every other tag older than 90 days and
   beyond the newest ten, every week. The API gave only the first two on 27
   and on 28 September 2026, though this item said all five were set; the
   maintainers added the other three through the API on 28 September, before
   the first image.
2. **Protected container tags**: one rule, for
   `^(latest|stable|[0-9]+\.[0-9]+(\.[0-9]+)?)$`, push Maintainer, delete Owner,
   so that nobody but a maintainer can push the release's tags and nobody but
   the owner delete them. A pre-release's tag (`6.3.5-rc1`) and the signatures
   are outside it. Immutable tags are GitLab Ultimate.
3. **The switch, `HM_PUBLISH_IMAGE`, stays unset until the project has
   moved.** The maintainers decided on 27 September 2026 to move the project
   into the group `hmailserver` before the first image is pushed, so that the
   first image's address, `registry.gitlab.com/hmailserver/hmailserver`, is
   the one it keeps. A container repository's path always matches its
   project's (GitLab's documentation, *Container registry*, "Move or rename
   container registry repositories"), so an image pushed from the present path
   would be `registry.gitlab.com/progressiverobot/hmailserver` and would change
   its address with the move, under everyone who pulled or pinned it. The
   signatures are no reason to wait: 6.3.4's `SigstoreVerifier` already trusts
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
   This project's registry is enabled and holds no repository (the project's
   API, 28 September 2026). The move from the personal namespace
   `Progressiverobot` to the group `hmailserver` changes the top-level
   namespace, so by the first of those rules the registry may have to be
   switched off for the transfer (Settings, General, "Visibility, project
   features, permissions") and on again after it; nobody has tried the
   transfer yet.

   The project moves only after a release signed by
   this pipeline from the present path has shown the project's id
   ([UpdateFeed.md](UpdateFeed.md)), so that release is made with the switch
   unset: its image jobs refuse and the registry stays empty. After the move
   the maintainers add `HM_PUBLISH_IMAGE` with the value `1` as a project
   CI/CD variable (Settings, CI/CD, Variables; a `v*` tag's pipeline is for a
   protected ref, so it may be marked Protected), and start
   `container-image-deb` in the pipeline of the release whose image is to be
   pushed - one made before the move included, since the jobs are in every
   `v*` tag's pipeline and read the switch as they run. It is a project
   variable because the project refuses variables given to a pipeline or a
   schedule, so nobody turns the jobs on for one run. Nothing committed sets
   it: `build/check-release-chain.py` fails when a CI file does, when a rule
   of an image job reads it, and when `container-image-deb.sh` or
   `linux-image.sh` does not refuse with a non-zero exit while it is not `1`.
   The Compose file and the chart name the group's path;
   `container-image` pushes only at the group's path, and refuses the first
   path and any other, so a switch set before the move pushes nothing.

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
   has not signed anything: that needs a tag's ID token. A new version is a
   new tag and digest here, never an image from `ghcr.io`, and
   `build/check-release-chain.py` fails on a tag without its digest.
2. **Inno Setup 6 and the Artifact Signing Client Tools on bench150**, as
   administrator ([bench150](#bench150)). Done on 26 September 2026.
   `windows-packages-x64` built the unsigned x64 installer with that Inno
   Setup in pipeline 2887115157 (27 September 2026); `windows-installer` and
   `release-authenticode`, which run only for a `v*` tag, have not run.
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
   certificate names `https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`
   (`SigstoreVerifier.cpp`), or `https://gitlab.com/hmailserver/hmailserver//...`
   with the project's id, the group the project is to move to, and Fulcio
   issues that certificate to any pipeline of such a tag, running whatever
   `.gitlab-ci.yml` the tag's commit has. Anyone who can push a `v` tag could
   therefore sign a release the servers accept with a CI file of their own,
   without the release jobs. The pipeline cannot enforce this: without the rule
   the release jobs simply do not run, and a hostile file does not need them.
   Check it again whenever the project's members or settings change, and after
   the move to the group, whose members and Owners can reach the project too.

The feed's pipeline needs nothing here. `update-feed` passes no variables, so
the project's *Minimum role to use pipeline variables* can stay at GitLab's
default for a new project, *No one allowed*; it is told apart from other
pipelines on `master` by its source, `pipeline`.

### What only a first run can show

Nothing here has run. These are written from the documentation and have not
been seen working:

- that a job which `needs` a manual job waits for it rather than being skipped
  (GitLab documents a blocking manual job, `allow_failure: false`, as holding
  the jobs after it, which is why both buttons are blocking);
- that GitLab's Fulcio certificate carries the repository id, which the notes
  report either way;
- that the gcr.io `-dev` tag chosen exists and that cosign reads
  `SIGSTORE_ID_TOKEN` from the environment in it, as GitLab's own documentation
  shows for its keyless example;
- that the Artifact Signing plugin's credential chain includes the workload
  identity credential (`AZURE_FEDERATED_TOKEN_FILE`) - the job exchanges the
  token itself first, so that a mismatch is reported in Entra's words, but
  SignTool's own exchange is the plugin's;
- that `update-feed` may trigger a pipeline in its own project with the job
  token, that the pipeline's source is `pipeline` (GitLab's documented source
  for a multi-project pipeline, which a `trigger: project` of the same project
  makes), and that GitLab Pages deploys from that pipeline;
- that the generic package upload answers with `file_sha256`, which the job
  does not rely on: it reads the registry back instead;
- that the job token may list the project's packages and their files, which
  `release` now does before anything is signed and `release-publish` before
  and after its upload;
- that fetching `master` in the job's shallow checkout brings its history down
  to the tagged commit, so that the ancestry check answers yes for a tag on
  `master`: seen with git 2.50 over `file://`, against a scratch copy whose
  `master` was this tree's branch (the real `master` has no `.gitlab/allowed_signers` yet),
  with the real `v6.3.4` tag passing and an unsigned tag off `master` refused;
  not on GitLab's runners;
- that GitLab's registry takes `container-image`'s push with the job's own
  registry credentials. The push itself - three tags naming one digest for
  `v6.3.1`, one tag for `v6.3.1-rc1`, the checks on what was pushed, the dotenv
  report, and the refusals - was run on linix on 26 September 2026, in the job's
  kaniko image, against a local registry standing in for `registry.gitlab.com`;
- that the dotenv report reaches `container_scanning`, which names the job in
  `dependencies` where the template leaves it empty. The analyzer, at the digest
  pinned here, scanned an image named by its digest (Ubuntu 24.04's, the
  Dockerfile's base) on linix on 26 September 2026 and wrote its report;
- that cosign stores the image's signature in GitLab's registry as
  `sha256-<digest>.sig` with the job's credentials, and that `cosign verify`
  finds it there;
- that the AArch64 package builds' record of what they were built against is
  what `build/asset-sbom.py` reads: `dpkg-query -W` in Ubuntu 26.04's
  container, with the cross toolchain's `libc6-dev-arm64-cross` and
  `libstdc++-15-dev-arm64-cross` beside the `:arm64` -dev packages, and
  `rpm --root` over the aarch64 sysroot dnf installed in Fedora 44's. The
  readers are tested against records written to that form; neither job has run
  with the recording, so the SBOM jobs have not either.

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

| What | Where it is named | Renovate's manager | Where new versions are read (datasource) | How it arrives |
|---|---|---|---|---|
| NuGet, the SDK-style projects | the Control Panel, the tools, their tests, `LinuxRegressionTests` | `nuget` | `nuget`: api.nuget.org | "NuGet packages and the .NET SDK", weekly, with the lock files |
| The .NET SDK | `global.json` | `nuget` | `dotnet-version`: Microsoft's release metadata | in the NuGet merge request; never a new major version |
| NuGet, the `packages.config` projects | RegressionTests, StressTest, VMTestRunner.Console | a regex manager | `nuget`: api.nuget.org | waits on the dashboard |
| Container images | `build/ci/linux.Dockerfile`, `.gitlab-ci.yml`, the packaging Dockerfile and compose file, `test/Docker/Dockerfile` | `dockerfile`, `gitlabci`, `docker-compose` | `docker`: Docker Hub and mcr.microsoft.com | "container images", weekly, every image pinned by digest; a new major version in a merge request of its own |
| npm and Python | nothing in the tree today | `npm`, `pip_requirements`, `pep621` | `npm`: registry.npmjs.org; `pypi`: pypi.org | one merge request each, weekly, once a manifest exists |
| OpenSSL | `hMailServer.vcxproj`, `hMailServer64.iss`, `post-build.bat` | a regex manager | `custom.openssl`: openssl-library.org/source/, the page only (its archive links point at GitHub and are never followed) | "native libraries", weekly, same major version |
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
project's path before its move, where no image is pushed), and
`hmailserver-ci/linux`, which is built on linix and never pushed. The .NET runtime the installer bundles has no pin
yet: `build/get-dotnet-runtime.ps1` takes the newest 10.0 by channel. A placeholder
rule in `renovate.json` says what to add when the supply-chain row pins it.

### How updates arrive

- **Once a week, one merge request per ecosystem.** The schedule runs the job
  every day, and `renovate.json` lets routine updates through on Mondays (London
  time) only. It allows the whole day, so a gate that holds linix all night does
  not cost a week. An open merge request is not refreshed on other days either
  (`updateNotScheduled` off). A new major version, where one is allowed, comes in
  a second merge request of its own, never beside routine bumps.
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
- **What stays put.** OpenSSL, Boost, libpq, zlib and the MariaDB client stay on
  their major version, and so do the .NET SDK and Microsoft's SDK image, which move
  with `global.json`. The container image's Ubuntu follows the `.deb` builder by
  hand. A rule in `renovate.json` holds each one, and changing that rule is the way
  to take the upgrade.
- **The dependency dashboard.** An issue, "Dependency updates (Renovate)", lists
  everything pending. Some updates wait there for a tick, because Renovate cannot
  make the upgrade: the `packages.config` packages, zlib, QuickJS, the MariaDB
  client and 7-Zip.
- **A new release waits three days** before it is proposed (NuGet, npm, PyPI), so
  a release that is pulled soon after publication is never offered. Security fixes
  do not wait.
- **Few pipelines.** A Renovate branch is rebased only when it conflicts. Its
  merge request runs `checkers` and `harnesses` on the shared runners, like any
  other. A `renovate/*` branch is not protected, so no job of its reaches linix or
  bench150.

### What a merge request still needs from a person

Each merge request carries its own note of these steps.

- **NuGet: restore on Windows before merging.** Renovate rewrites the lock files
  with a restore on Linux. For the projects that target `net10.0-windows`, a Linux
  restore lists packages the Windows Desktop runtime already carries
  (`System.CodeDom`, `System.Diagnostics.EventLog`), which a restore on Windows
  leaves out. On 25 September 2026 a Linux restore of an untouched `master`, with
  the SDK `global.json` names, rewrote the ControlPanel and DBSetup lock files that
  way. On Windows, on the branch, run
  `dotnet restore hmailserver/source/Tools/ControlPanel.sln` and
  `dotnet restore "hmailserver/source/Tools/hMailServer Tools.sln"`, and commit
  what they write. Otherwise the Windows gate's build rewrites them on bench150 and
  leaves the bench's checkout modified.
- **A new SDK in `global.json`**: install it on every Windows build machine and
  on both benches first. Land the container-images merge request that moves the
  SDK image, and rebuild the Linux image, before it too.
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

How long each job takes
-----------------------

**Written before any job had run.** The first pipeline (2884863496, 26
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
| `chart-and-compose`, `update-helper`, `package-templates`, `roadmap` | 1 to 30 s by hand | 5 to 10 min | Plus the image pull on a shared runner: the .NET SDK image is about 1.3 GB |
| `linux-build` | 12 min by hand | 1 h | The clang build took 695 s with six jobs; the testers, the OpenPGP round trip and the packages take seconds |
| `linux-suite` | 2 to 2.5 h | 4 h | The whole suite took 75 to 90 minutes on GitHub's hosted runner on 16 September 2026, and it has grown since: the job took 2 h 7 min in the first GitLab pipeline, on 26 September 2026, before the 29 fixtures linked that day for the coverage programme, which took 20 minutes more in a run of their own on linix, against the instrumented server. The timeout was three hours until then; it is the runners' maximum now, so that a suite that grows every batch is not cut off |
| `linux-cross` | 8 min by hand | 1 h | 750 translation units compiled for AArch64 with six jobs |
| `linux-sanitize` | seconds by hand, while it skips | 1 h 30 min | Once `HM_SANITIZE` exists, an estimated 15 to 25 minutes: the core under AddressSanitizer, RelWithDebInfo |
| `linux-db-upgrade-pgsql`, `-mysql` | 16 s and 31 s on the first pipeline, which stopped at 6101 | 20 min | 75 upgrade steps from 6029, and a new database made to compare |
| `linux-package-deb`, `-rpm` | 17 s and 2 s by hand | 20 and 10 min | |
| `linux-image` | 18 s by hand | 30 min | kaniko over Ubuntu 24.04 and the `.deb` |
| `linux-sbom` | 6 s by hand | 15 min | |
| `linux-repro` | 11 to 16 min by hand | 1 h | One more run of `linux-build.sh` - the build, the testers and the packages - plus a copy of the tree and the comparison, which take seconds: the second build took 760, 795, 959, 673, 686 and 696 s with six jobs in six runs on linix on 26 September 2026 |
| `windows-gate` | 2.5 to 3 h | 4 h | On 25 September 2026 the two halves took 62 minutes (1,339 tests, on bench150) and 70 minutes (2,126 tests, on the laptop), each with its build. One bench runs both, one after the other, with one build. Add any wait for the lock |
| `windows-dotnet` | 10 to 20 min, estimated | 4 h | Two restores (cold for LocalSystem the first time), two Release builds, MIDL, the tests and five publishes; the timeout allows 200 minutes of waiting for the lock |
| `linux-fuzz` | 65 min | 2 h | Three harnesses of 20 minutes, plus about 3 minutes to build and replay (by hand: 3.5 minutes all told with one minute a harness) |
| `linux-coverage` | about 3 h, estimated | 4 h | By hand on linix on 26 September 2026, in the job's image: the whole suite against the instrumented server took 126 minutes, and the 29 fixtures linked that day 20 minutes more in a run of their own; the testers took under a minute and the report seconds. The instrumented build was not timed on its own; `linux-build`'s took 20 minutes in the first pipeline |
| `static-analysis` | 35 to 50 min | 90 min | Its script, run by hand on linix on 25 September 2026 in the job's image with four CPUs where the job has six, beside other work: 51 min 28 s, of which cppcheck took 2 min 22 s and clang-tidy 49 min for 741 files |
| `semgrep` | 3 to 5 min | 30 min | The same, with one CPU: 2 min 36 s to 2 min 50 s, the install and the rules' download included |
| `ci-image-packages` | under a minute | 10 min | The copy takes a second; the rest is the job's start |
| `dependency-scan` | 1 to 2 min | 30 min | The same, with one CPU: 15 to 17 s, the vulnerability database's download included |
| `repo-hygiene` | 1 min | 10 min | The same, with one CPU: 14 to 37 s, the apt install included |
| `semgrep-sast` | 5 to 10 min, estimated | 30 min | It passed in 96 s in `master`'s pipeline 2886389460 (27 September 2026), and counts against the namespace's compute minutes on GitLab's shared runners. By hand on linix on 27 September 2026, over batch41 in the job's image pinned to two CPUs as a shared runner has: 332 s, with the whole of every file read (84 s before `.gitlab/ci/gitlab-security.yml` gave a rule two minutes and lifted the size limit, when it read neither `RestApiServer.cpp` nor `Portal.js`) |
| `secret_detection` | 17 s in a pipeline | 30 min | In `master`'s pipeline 2886389460 (27 September 2026) it failed on twenty-four vetted test fixtures, which `.gitleaksignore` names since `535e9005c`. It counts against the namespace's compute minutes on GitLab's shared runners. Its scan of the whole history took under a minute on linix |
| `container-image` | under a minute by hand | 30 min | kaniko over Ubuntu 24.04 and the `.deb`, pushed to a local registry standing in for GitLab's, on linix on 26 September 2026; the push to `registry.gitlab.com` adds the upload of about 40 MB |
| `container-image-sign`, `container_scanning` | not measured | 15 and 30 min | Neither has run in a pipeline. The analyzer's scan of Ubuntu 24.04 on linix, by hand, finished within two minutes of its start, its image's download included |
| `renovate` | 2 to 5 min | 1 h | The dry runs of 25 September 2026 on linix: 5 s for the advisory database, 9 s to install the .NET SDK, 1 to 1.5 min for Renovate. The first run also pulls its image, about 450 MB |

The runners' own `maximum_timeout` is four hours. GitLab lets a job's
`timeout:` exceed the project's default of one hour, but not exceed its
runner's maximum. linix runs one job at a time, so its jobs add up: about 12
minutes of build, 2 to 2.5 hours of suite and about 10 minutes of the rest, so
three hours or a little more, and the security scans add about an hour to
that ([What they cost](#what-they-cost)). bench150's two jobs add up the same way. So a
pipeline for a protected ref takes about three hours from start to finish, and
the Windows gate and the Linux lane decide it between them.

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
A release is created on GitLab after its tag is pushed, so the tag's own run
of `pages` misses it. A release made by the release jobs ends with
`update-feed`, which starts a pipeline on `master` running `pages` and `package-repos` alone; the
schedule brings the feed up to date after a release made or edited any other
way.
Until there is one, running a pipeline on `master` by hand after a release is
published does the same (Build, Pipelines, Run pipeline), along with every other
`master` job.

**A schedule's pipeline runs `pages` and `package-repos` alone, and so does the feed's.** A
scheduled pipeline is a pipeline on `master` to every job in the file, and so
is the downstream pipeline a release's `update-feed` job starts, whose
`CI_PIPELINE_SOURCE` is `pipeline`, so each of the others says it is not for
one: `.own-runners` (and so every job on linix and bench150) and
`.not-a-fuzz-pipeline` (and so `checkers`, `harnesses` and `editorconfig`)
begin with `- if: $CI_PIPELINE_SOURCE == "schedule" || $CI_PIPELINE_SOURCE ==
"pipeline"` with `when: never` - any schedule and the feed's pipeline - and the
four path-bound checks, `db-matrix`, the release jobs and GitLab's two scanners
carry the same rule. The feed's pipeline is told apart by its source rather
than by a variable because a trigger job's variables reach the pipeline it
starts as pipeline variables, which GitLab refuses by default in a project made
since 17.7, and the trigger would fail. `build/check-release-chain.py` fails on
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
  containers' `/tmp` too. `linux-fuzz` leaves its grown corpus in the runner's
  cache volume on linix, on purpose, and `fuzz/bin`, `fuzz/build` and
  `fuzz-results/` in the checkout until the next job cleans it; its artifact
  expires after 30 days. The others' artifacts expire after three and seven
  days.
- **`linux-db-upgrade-*`** and **`linux-package-deb`** leave nothing: their
  databases are in the service containers, which the runner removes. The
  package jobs install into their own job containers.
- **`linux-image`** removes its tarball before it ends and keeps only the
  image's configuration and hashes (`linux-image/`, seven days). kaniko's
  layers are in its own container.
- **`linux-sbom`**'s two documents are its artifact, kept 90 days.
- **`linux-repro`** copies the tree to `/tmp/hm-repro-source` and builds in
  `/tmp/hm-repro-second-build`, both in its container, and removes both before
  it compares. Its artifact, `linux-repro/` (the report, the second build's
  logs, and its binary and packages only when they differ from
  `linux-build`'s), is kept 30 days.
- **`linux-suite`** lays its tree out inside the checkout (`.hmtest-tree`),
  because `after_script` may run in a new container from the same image, which
  sees only the checkout. `after_script` copies the server's error logs (and every log,
  compressed, when the job did not pass) into `linux-suite/` and removes the
  tree. It does so for a cancelled job too, and for one that ran out of time
  (`RUNNER_SCRIPT_TIMEOUT`). The runner removes the PostgreSQL service
  container when the job ends; `docker volume prune` on linix clears any
  anonymous data volume it leaves. `linux-suite/` stays until the next job's checkout cleans it. The
  artifact expires after seven days.
- **linix** keeps the image, each older tag of it until somebody removes it,
  `postgres:16`, `mariadb:11`, `ubuntu:26.04`, `fedora:44`, the kaniko image,
  the runner's helper image and its build and cache volumes.
- **`windows-dotnet`** leaves its clone in `C:\GitLab-Runner\builds` with the
  build output in it (the next job's checkout cleans that), the NuGet packages
  in LocalSystem's profile, which the next run reuses, and its TRX files as the
  artifact for seven days. No MSBuild or compiler server is left running.
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
  `python:3.13-slim-trixie` and `aquasec/trivy:0.74.0`, at their digests. Their
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
rule, not a database. Every job is `allow_failure: true` until its first
findings are triaged, so none of them can fail a pipeline yet; but four of them
hold a release ([Before a release](#before-a-release-the-scans-hold-it)).

Two more come from GitLab's own templates (`.gitlab/ci/gitlab-security.yml`,
included from `.gitlab-ci.yml`): `semgrep-sast`, GitLab's Semgrep analyzer and
rules over every language of the tree, the server's C and C++ included, and
`secret_detection`, which looks for a key, token or password in the commits a
pipeline brings. They run on GitLab's shared runners, for `master` and a merge
request, and `semgrep-sast` for a `v*` tag too, whose release job reads its
report, in images from `registry.gitlab.com` pinned by digest. Each
leaves its JSON report for Developers and above to download.
`SAST_DISABLED=true` or `SECRET_DETECTION_DISABLED=true`, as a CI/CD variable,
turns one off. A third, `container_scanning`, scans the container image a `v*`
tag's release pushes, after the push, in the same way
([The container image](#the-container-image)); `CONTAINER_SCANNING_DISABLED=true`
turns it off.

`semgrep-sast` is `allow_failure`, as its template makes it, and holds nothing
to a baseline. **`secret_detection` fails on a finding.** The template cannot:
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
in, while `master` is pushed with `ci.skip`. The owner makes it once: *Build*,
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

**Proved by hand, not yet in a pipeline.** Each job's script was run on linix
on 25 September 2026, in a container of the job's own image, with the job's own
commands, on a copy of the tree checked out as GitLab checks it out (CRLF, the
scripts LF). The dependency scan was run with `ghcr.io`, `github.com` and
GitHub's content hosts resolved to 127.0.0.1, and it completed. The numbers
below come from those runs. What they found is in the two baselines:

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

`allow_failure` keeps a finding from stopping a push's pipeline. It does not let
one into a release. A `v*` tag's `release` job needs four scans of the tagged
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
  that the VEX document does not dismiss;

and `.gitlab/security-exceptions.yml` does not accept that finding - `master`'s
copy, which the job reads as it reads `master`'s allow-list, never the tagged
tree's, so a finding the owner accepts on `master` after the tag counts when the
job is retried. An entry there names the scan and the finding (a fingerprint; for `dependency-scan` the
advisory's id and the package), what it is, why it is accepted, the GitLab user
who read it, the day, and an expiry at most a year after it. After the expiry
the finding holds the release again until it is read again. The file is a strict
subset of YAML (the job's image has no YAML library), is the owner's in
`.gitlab/CODEOWNERS`, and `build/check-release-chain.py` reads it on every
pipeline, so a malformed entry fails `checkers` long before a release. It
holds 169 entries today, all `semgrep-sast` (below). The thresholds are
`SECURITY.md`'s ("Vulnerability management policy"); the gate's self-test, run
by `checkers`, holds it to each of them.

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

This is a gate on releases, not on changes: the scans still run for protected
refs only, after the push, and `master` is pushed with `ci.skip`. A finding a
change brings stops the next release until it is fixed or accepted, not the
change.

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
- **The landing.** The maintainer's landing script (his own tool, not in the
  tree) scans every commit a batch brings to `master`
  (`build/scan-secrets.py --range master..<tip>`) and pushes nothing when it
  finds a secret, or when the scan cannot run.
- **`secret_detection`**, above, fails the pipeline of a merge request, or of a
  push to `master`, on a finding `.gitleaksignore` does not name - after the
  push, and not for a push to another branch without a merge request. `master`
  is pushed with `ci.skip` today, so for `master` the landing's scan is the one
  that runs.
- **`.gitignore`** keeps the usual key and credential files (`*.pem`, `*.key`,
  `*.pfx`, `*.p12`, SSH private keys, `.env`, `.netrc` and the like) out of a
  `git add`, but for the test fixtures' directories, which hold throwaway keys on
  purpose.

`build/scan-secrets.py` runs gitleaks from the image `secret_detection` pins -
it reads the digest from `.gitlab/ci/gitlab-security.yml`, so the two cannot
differ - with GitLab's rules and the repository's `.gitleaksignore`: in Docker on
the machine, or in Docker on another machine that sees the repository's git
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

### The old GitHub jobs and what replaced them

| On GitHub | What it did | On GitLab | What is not replaced |
|---|---|---|---|
| `codeql.yml`, C++ | CodeQL `security-extended` over the MSVC build of the server, on every push and pull request (40 to 54 minutes) | `static-analysis`: cppcheck, and clang-tidy with the Clang static analyser, over every translation unit the Linux build compiles | The Windows-only sources (the COM API, the service host, the ADO and SQL Server Compact backends, the registry) and the Windows arm of every `#ifdef`, which only an MSVC build compiles; CodeQL's interprocedural taint tracking. `build\analyze.ps1` (MSVC `/analyze`) still covers the Windows build by hand |
| `codeql.yml`, C# | CodeQL `security-and-quality` over the Control Panel and the setup and migration tools | `semgrep`: Semgrep's open C#, Python, JavaScript and bash rules. It also reads the Python tools, the shell scripts, the webmail's scripts and the Control Deck's page, which CodeQL did not | CodeQL's type-aware queries and its quality queries |
| `dependency-review.yml` | Failed a pull request that added a dependency with a high or critical advisory, or a package flagged as malware | `dependency-scan`, fed by `ci-image-packages`: Trivy over the lock files and the CI image, OSV over the vendored libraries. It fails on a high or critical advisory that has a fixed version | The gate at merge-request time: a merge request from a fork never reaches linix. GitHub's malware advisories |
| Dependabot | Alerts, and a pull request for each advisory and each new version of an action | The alerts are `dependency-scan`'s | The update pull requests. A bot that opens merge requests (Renovate runs on GitLab) needs a bot account and a token, and that is a decision of its own |
| `scorecard.yml` | OpenSSF Scorecard, published to the OpenSSF API and the README's badge | `repo-hygiene`: Branch-Protection, Signed-Releases, Security-Policy, Pinned-Dependencies, Binary-Artifacts and Token-Permissions, answered from GitLab's public API and the tree | The checks only GitHub can answer, and the published score (below) |
| Secret scanning with push protection | Refused a push that carried a live secret | `secret_detection`, GitLab's free pipeline secret detection, on the shared runners for `master` and a merge request: it finds a secret after the push and fails the pipeline; the `security` schedule scans the whole history | The refusal. Push protection is a GitLab Ultimate feature |

### The jobs, and where each tool comes from

| Job | Image | Tools, and where they come from | Reports | Measured on linix |
|---|---|---|---|---|
| `static-analysis` | the CI image | cppcheck 2.19.0 and clang-tidy 21.1.8, from Ubuntu 26.04's archive, installed as the job starts. clang-tidy is the image's own clang 21 | `gl-sast-report.json` (both tools; each tool's own beside it), `junit.xml` | 51 min 28 s on 4 CPUs |
| `semgrep` | `python:3.13-slim-trixie`, pinned by digest (Docker Hub) | Semgrep 1.178.0 and 65 packages it needs, from PyPI, with every file's SHA-256 pinned (`build/ci/requirements.txt`, `pip --require-hashes`). The rules come from Semgrep's registry at semgrep.dev (`p/csharp`, `p/python`, `p/javascript`, `r/bash`), fetched with no account and no token | `gl-sast-semgrep.json`, `junit.xml` | under 3 min on 1 CPU |
| `ci-image-packages` | the CI image | none | the image's `etc/os-release`, `etc/lsb-release`, dpkg status and the .NET SDK's `deps.json` files, 1.4 MB | seconds |
| `dependency-scan` | `aquasec/trivy:0.74.0`, pinned by digest (Docker Hub) | Trivy 0.74.0; its vulnerability database from `mirror.gcr.io`, with `public.ecr.aws` as the fallback; OSV's API at `api.osv.dev`; Python from Alpine's repository | `gl-dependency-scanning.json` (both scanners; each scanner's own beside it), `junit.xml` | 15 to 17 s, the database download included |
| `repo-hygiene` | `python:3.13-slim-trixie`, pinned by digest | git and ssh-keygen, from Debian; GitLab's API, read anonymously | `junit.xml`, `repo-hygiene.json` | 14 to 37 s, the apt install included |

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
  test case per tool that fails when anything is new beyond the baseline, one
  that fails when a file could not be analysed, and one failing test case per
  new finding. `dependency-scan` writes one test case per lock file, for the CI
  image and for each vendored library, skipped when the answer is unknown.
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
`build/ci/security-findings.py` compares the fingerprints and exits with:

- 0 when nothing is new;
- 1 when something is new;
- 2 when the scan has a hole: a tool that did not finish, a file cppcheck or
  clang-tidy could not parse, or a Semgrep rule that timed out on a file. A hole
  is never a pass, and no baseline excuses it.

So the day `allow_failure` comes off a job, a new finding fails the pipeline and
an old one does not.

To triage a new finding:

1. Read it: the job log, `new-findings.txt`, or the failing test case in the
   *Tests* tab.
2. If it is a defect, fix it in the change that brought it.
3. If it is a false positive, suppress it where it is, with the reason in a
   comment beside it: `// cppcheck-suppress <id>`, `// NOLINT(<check>)` or
   `# nosemgrep`. A kind that recurs across the tree - a `snprintf` whose
   return is not read, a product of constants - may go into the baseline
   instead, with its reason, as in step 4.
4. If it is real but not to be fixed now, regenerate the baseline in a commit
   that says why, and give the regeneration `--reasons`: a JSON file of
   `{"<fingerprint>": "<why>"}` and `{"rule:<rule>": "<why>"}` for the
   findings new to the baseline. Each one's line then ends with its reason,
   and a reason stays with its finding when the file is written again.

To regenerate a baseline, run the job's script by hand on linix with
`--write-baseline`, from a checkout of the commit:

    docker run --rm -v "$PWD":/src -w /src hmailserver-ci/linux:<tag> bash build/ci/static-analysis.sh --write-baseline
    docker run --rm -v "$PWD":/src -w /src python:3.13-slim-trixie bash build/ci/semgrep.sh --write-baseline

Or take a job's artifacts, which hold the tools' raw output, and run
`build/ci/security-findings.py --write-baseline` over them. Its help gives the
command, and nothing is scanned again. This is the way to give `--reasons`,
which the job scripts do not pass on; it then says how many findings were new
to the baseline and how many of those came without a reason, so that one
nobody triaged shows. A line may be deleted from a baseline by
hand when its finding is fixed. The job reports how many of the baseline's
findings it no longer sees.

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
reads that document (`--vex`), and so does the OSV step. A dismissed finding is
still listed, marked as dismissed.

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

The CI image's scan finds Ubuntu 26.04 with 259 packages and 3,024 advisories.
2,953 of those are kernel advisories filed against `linux-libc-dev`, the kernel
headers that `build-essential` pulls in, which run no kernel code. One of the
3,024 has a fixed version today: CVE-2026-57062, rated low, in `gpgv`. A rebuild
of the image (`build/ci/build-linux-image.sh`) takes Ubuntu's fixes when they
come.

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
what it cannot read it refuses rather than passes: a rules list given inline
or by an alias, a merge key (`<<:`), an `extends` by an alias, an `!reference`
the files do not resolve, and an include that is not a local file or one of
GitLab's templates. Tags and `extends` may be block lists. A job is on the
project's runners when it asks for a tag that is a variable, or for any tag
they are registered with (`linix`, `linux` and `docker` for linix; `bench150`,
`bench` and `windows` for bench150). `id_tokens`, `secrets` and a credential
in a template count for every job that extends it or takes a part of it by
`!reference`. `python3 build/ci/repo-hygiene.py --self-test`, which the
`checkers` job runs, holds the check to CI files made to pass it and to 23
made to break it; the check as first rewritten (a4636e983) passed 22 of those
23, and failed two of the eight that should pass. Both runners are registered
for protected refs only (`access_level` `ref_protected`, read by a maintainer
with `glab api runners/<id>` on 26 September 2026), so none of that was open
on this project: the check is the CI files' own guarantee.

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
  the account (Fuzzing.md).
- **License.** `LICENSE` is at the root.
- **Dependency-Update-Tool.** It could be answered, and it would fail, as the
  table above says.

The published score and its README badge are GitHub-only. The OpenSSF API
accepts a result only from a GitHub Actions run.

### What else was left out, and why

- **CodeQL itself.** Its command-line tool and its query packs are published
  only on GitHub: the tool as release assets on github.com, the packs on
  `ghcr.io`.
- **OSV-Scanner.** It is published on GitHub and `ghcr.io` only. Neither Debian
  nor Ubuntu packages it (apt knows no `osv-scanner` in trixie or 26.04). The
  OSV step calls OSV's API directly instead.
- **Scorecard's own command-line tool.** It reads GitLab too, but its one
  channel that is not GitHub, `gcr.io/openssf/scorecard`, answered "This API
  method requires billing to be enabled" on 25 September 2026. Its other
  channels are GitHub's release assets and `ghcr.io`.
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

linix takes one job at a time, so the five jobs add their time to linix's share
of a protected pipeline: 35 to 50 minutes for `static-analysis` (clang-tidy's
static analyser is most of it) and about ten for the rest. linix's share becomes
about 2.5 to 4 hours, against the Windows gate's 2.5 to 3 hours on bench150, so
linix may now be the longer of the two. If it is, the first lever is
`static-analysis` on `master` and `v*` only. A batch's pipeline
runs again on `master` when the batch lands, so a new finding is still seen
there. Making that change is one `rules:` line.

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
- The Linux build under GCC and on AArch64 hardware
  (`bash build/linux-tu-census.sh --cc g++` is the GCC census). The AArch64
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
- Building the Linux job image when its Dockerfile changes (see [The
  image](#the-image)), and creating the fuzz schedule once (see [The fuzz
  schedule](#the-fuzz-schedule)).
- Bringing a runner back when its machine has been rebuilt (see
  [Re-creating a runner](#re-creating-a-runner)).
