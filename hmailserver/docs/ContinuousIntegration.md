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

**No GitLab job has run yet.** When this was written, neither runner had taken a
job. The ported Linux jobs' scripts were run by hand on linix, in the jobs' own
images and beside the same services, and the times they took are given as
measured by hand. Every other duration below is an estimate made from the gates
run by hand, and says so. Replace both with the first runs' times.

What runs where
---------------

| Job | Stage | Runs on | Runs for | What it does |
|---|---|---|---|---|
| `checkers` | check | GitLab's shared runners | every pipeline but a schedule's | Every Python checker, the self-tests, the generators' `--check` ([CONTRIBUTING.md](../../CONTRIBUTING.md), "The checkers") |
| `harnesses` | check | GitLab's shared runners | every pipeline but a schedule's | The Control Deck's and the webmail's script harnesses, and the S/MIME and OpenPGP checks |
| `sign-off` | check | GitLab's shared runners | a merge request from a fork | Every commit carries its author's `Signed-off-by` |
| `editorconfig` | check | GitLab's shared runners | every pipeline | Every text file against `.editorconfig`, and a negative control |
| `chart-and-compose` | check | GitLab's shared runners | a change to the Helm chart or the Compose file | The chart linted and rendered, the Compose file resolved |
| `update-helper` | check | GitLab's shared runners | a change to the update helper or the units | `build/test-hmailserver-update.sh` under dash, and `systemd-analyze verify` of the three units |
| `package-templates` | check | GitLab's shared runners | a change to the winget or Chocolatey templates | The templates render and agree, checked offline |
| `roadmap` | check | GitLab's shared runners | a change to `Roadmap.md` | `build/check-roadmap.ps1` |
| `linux-build` | build | linix | a protected ref | The Linux server with clang, every `*-tests` program the CMake build makes, the OpenPGP writer against GnuPG, and the `.deb` and the `.rpm` (`build/ci/linux-build.sh`) |
| `pages` | release | GitLab's shared runners | `master`, a `v*` tag, a schedule on `master` | Renders the update feed from the project's releases and GitLab Pages publishes it at https://updates.progressiverobot.com (see [The update feed](#the-update-feed)) |
| `linux-suite` | test | linix | a protected ref | The whole of `LinuxRegressionTests` against that server on PostgreSQL 16 (`build/ci/linux-suite.sh`) |
| `linux-cross` | test | linix | a protected ref | Every core translation unit compiled for AArch64 (`build/ci/linux-cross.sh`) |
| `linux-sanitize` | test | linix | a protected ref | `snmp-tests` under AddressSanitizer; says SKIPPED and passes while the tree has no `HM_SANITIZE` (`build/ci/linux-sanitize.sh`) |
| `linux-db-upgrade-pgsql`, `linux-db-upgrade-mysql` | test | linix | a protected ref | A 6029 database with orphaned rows upgrades to this schema, on PostgreSQL 16 and MariaDB 11 (`build/ci/linux-db-upgrade.sh`) |
| `linux-package-deb`, `linux-package-rpm` | test | linix | a protected ref | The packages installed in clean Ubuntu 26.04 and Fedora 44 containers; the `.deb` configured, started and asked for the Deck (`build/ci/linux-package-install.sh`) |
| `linux-image` | test | linix | a protected ref | The container image built from the `.deb` with kaniko and inspected, not pushed (`build/ci/linux-image.sh`) |
| `linux-sbom` | test | linix | a protected ref | The SPDX and CycloneDX SBOMs of the tree, kept 90 days (`build/ci/linux-sbom.sh`) |
| `db-matrix` | test | linix | a protected ref, started by hand | The `linux-build` server against MySQL 8.4, MariaDB 11.4 or PostgreSQL 17 in the test lab (`HM_LAB_DB`), the database-sensitive fixtures or the whole suite (`HM_SUITE_FILTER`); `build/lab/db-suite.sh`, [TestLab.md](TestLab.md) |
| `windows-gate` | test | bench150 | a protected ref | The whole Windows suite in the bench's own checkout (`build/ci/windows-gate.ps1`) |
| `windows-dotnet` | test | bench150 | a protected ref | The Control Panel and the tools built with warnings as errors, published and tested, and `dotnet format` (`build/ci/windows-dotnet.ps1`) |
| `linux-fuzz` | test | linix | the schedule described `fuzz`, and nothing else | The three MIME harnesses under libFuzzer (`build/ci/linux-fuzz.sh`); its findings are its artifact, for Developers and above |
| `static-analysis` | test | linix | a protected ref | cppcheck and clang-tidy over the server's C++, held to a baseline ([Security scanning](#security-scanning)) |
| `semgrep` | test | linix | a protected ref | Semgrep's open rules over the C#, Python, JavaScript and shell sources, held to a baseline |
| `ci-image-packages` | test | linix | a protected ref | Copies the CI image's package database out, for `dependency-scan` |
| `dependency-scan` | test | linix | a protected ref | Trivy over the lock files and the CI image, OSV over the vendored libraries |
| `repo-hygiene` | test | linix | a protected ref | The repository checks OpenSSF Scorecard ran, answered from GitLab and the tree |
| `semgrep-sast` | test | GitLab's shared runners | `master`, and a merge request | GitLab's own SAST template: its Semgrep analyzer and rules over the C, C++, C#, Python and JavaScript, with no baseline (`.gitlab/ci/gitlab-security.yml`) |
| `secret_detection` | test | GitLab's shared runners | `master`, a merge request, and the schedule described `security` | GitLab's own secret-detection template: a key, token or password in the commits the pipeline brings, or in the whole history for the `security` schedule; a finding `.gitleaksignore` does not name fails it (`.gitlab/ci/gitlab-security.yml`) |
| `windows-installer` | release | bench150 | a `v*` tag, after `windows-gate` | The Windows installer, unsigned, built in the bench's checkout (`build/ci/windows-installer.ps1`; [The release](#the-release)) |
| `release` | release | GitLab's shared runners | a `v*` tag, started by hand | Checks the tag, the version stamp and the release settings, and collects the six files a release carries (`build/ci/release-chain.py collect`) |
| `release-authenticode` | release | bench150 | a `v*` tag, after `release` | The installer Authenticode-signed through Azure Artifact Signing with the job's ID token (`build/ci/windows-authenticode.ps1`) |
| `release-sign` | release | GitLab's shared runners | a `v*` tag, after `release-authenticode` | Every file signed with Sigstore as this pipeline (`build/ci/release-sign.sh`) |
| `release-verify` | release | linix | a `v*` tag, after `release-sign` | Every bundle checked with this commit's `hmailserver --verify-update` (`build/ci/release-verify.sh`) |
| `release-publish` | release | GitLab's shared runners | a `v*` tag, started by hand after `release-verify` | The files uploaded to the package registry and the GitLab release created (`build/ci/release-chain.py publish`) |
| `update-feed` | release | (a trigger) | a `v*` tag, after `release-publish` | Starts a downstream pipeline on `master`, passing no variables, which runs `pages` alone |
| `installer-smoke` | release | GitLab's shared runners | a `batch*` branch or a `v*` tag, started by hand | A placeholder. It fails with a sentence saying that the installer smoke test runs from the VMware host ([The installer smoke test](#the-installer-smoke-test)) |
| `renovate` | check | linix | the schedule described `renovate`, and nothing else | Renovate's dependency updates ([Dependency updates](#dependency-updates)) |

`linux-build`, `windows-gate`, `windows-dotnet`, `linux-cross`, `linux-sanitize`
and `linux-sbom` start as soon as the pipeline does. They do not wait for the
checks, because the two machines are otherwise idle and the Windows gate is the
longest job. `linux-suite`, the two upgrade gates, the two package jobs and
`linux-image` wait for `linux-build`, whose artifact holds the binary and the
packages, and `release` waits for every job that can fail a tag's pipeline - the
checks, both suites, the upgrade gates, the package jobs, `linux-cross`,
`linux-sanitize`, `linux-image`, `windows-dotnet` - and `windows-installer`
([The release](#the-release)). linix runs one
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
| `master` | the branch's | all of them but the release jobs, `linux-fuzz` and `installer-smoke` |
| a `batch*` branch | the branch's | all of them but the release jobs, `linux-fuzz`, `pages`, `semgrep-sast` and `secret_detection`, with `installer-smoke` manual: this is the gate a batch passes before it lands |
| any other branch, no open merge request | the branch's | `checkers`, `harnesses`, `editorconfig`, and the four path-bound checks whose files changed |
| any other branch, with an open merge request | the merge request's | `checkers`, `harnesses`, `editorconfig`, the path-bound checks whose files the merge request changes, `semgrep-sast`, `secret_detection`, and `sign-off` when the branch is in a fork |
| a merge request from `master` or a `batch*` branch | none; the branch's own pipeline stands for it | as for the branch |
| a `v*` tag | the tag's | all of them but `linux-fuzz`, `semgrep-sast` and `secret_detection` (the commit's `master` pipeline ran them), with `release`, `release-publish` and `installer-smoke` manual ([The release](#the-release)) |
| any other tag | none | |
| the schedule described `fuzz`, on `master` | the schedule's | `linux-fuzz` alone ([The fuzz schedule](#the-fuzz-schedule)) |
| the schedule described `renovate`, on `master` | the schedule's | `renovate` alone ([Dependency updates](#dependency-updates)) |
| the schedule described `security`, on `master`, run once by hand | the schedule's | `secret_detection` alone, over the whole history ([Security scanning](#security-scanning)) |
| any other pipeline schedule on `master` (the update feed's, `update feed`) | the schedule's | `pages` alone (see [The update feed](#the-update-feed)) |
| the pipeline `update-feed` starts (on `master`, source `pipeline`) | a downstream pipeline | `pages` alone |

A schedule is told apart by its description, which GitLab gives the pipeline
as `CI_PIPELINE_SCHEDULE_DESCRIPTION`, compared whole: `fuzz`, `renovate` and
`security` are the three words that start a job of their own, and a schedule
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
allowed to Maintainers.

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
- For `windows-installer`: Inno Setup 6 installed for the machine (`winget
  install --exact --id JRSoftware.InnoSetup --scope machine`, as an
  administrator), because LocalSystem sees no per-user installation; and access
  to `gitlab.com`'s package registry, `7-zip.org` and Microsoft's .NET download
  site, which `get-installer-binaries.ps1` and `get-dotnet-runtime.ps1` fetch
  from, each file checked against its pinned hash. `HM_ISCC`, a CI/CD variable,
  names `ISCC.exe` where it is somewhere else. Not installed yet (26 September
  2026).
- For `release-authenticode`: Microsoft's Artifact Signing Client Tools
  (`winget install --exact --id Microsoft.Azure.ArtifactSigningClientTools
  --scope machine`), which bring SignTool's Artifact Signing plugin and the
  .NET 8 runtime it needs; SignTool itself is the Windows SDK's, which the Build
  Tools installed. `HM_SIGNTOOL` and `HM_SIGNING_DLIB` name either where it is
  somewhere else. Not installed yet (26 September 2026).

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
`linux-cross`, `linux-sanitize`, the two upgrade gates, `linux-sbom` and
`linux-fuzz` - run in one image, described by `build/ci/linux.Dockerfile`. The
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
history: all of them at `6399e08a5`, the last commit that had them, and the ones
that built and signed releases up to 6.3.3 at the tag `v6.3.3`.

| GitHub workflow and job | GitLab job | Runner | When |
|---|---|---|---|
| `ci.yml` control-panel | `windows-dotnet` | bench150 | a protected ref |
| `ci.yml` tools | `windows-dotnet` | bench150 | a protected ref |
| `ci.yml` upload-coverage-csharp | not ported: it fed GitHub's code-quality page, and the tests run without the coverage collector | | |
| `style.yml` editorconfig | `editorconfig` | shared | every pipeline |
| `style.yml` license-headers | `checkers` (row 139) | shared | every pipeline |
| `style.yml` dotnet-format | `windows-dotnet` | bench150 | a protected ref |
| `server-build.yml` | `windows-gate`, which builds the Release server before the suite | bench150 | a protected ref |
| `dco.yml` | `sign-off` (row 139) | shared | a merge request from a fork |
| `roadmap.yml` | `roadmap` | shared | `Roadmap.md` changed |
| `linux-build.yml` stamps | `checkers` runs `check-linux-version-stamp.sh` (row 139), and `linux-build` runs it again before it packages: a drifted stamp makes no package | shared, linix | every pipeline; a protected ref |
| `linux-build.yml` build, x86-64 clang | `linux-build` (with the packages) | linix | a protected ref |
| `linux-build.yml` build, x86-64 GCC | not ported: see below | | |
| `linux-build.yml` build, AArch64 | not ported: see below | | |
| `linux-build.yml` build, the AppImage | not ported: see below | | |
| `linux-build.yml` cross | `linux-cross` | linix | a protected ref |
| `linux-build.yml` package-install | `linux-package-deb`, `linux-package-rpm` | linix | a protected ref |
| `linux-build.yml` regression-on-linux | `linux-suite` (row 139), the whole suite in one run | linix | a protected ref |
| `linux-build.yml` db-upgrade | `linux-db-upgrade-pgsql`, `linux-db-upgrade-mysql` | linix | a protected ref |
| `linux-build.yml` image | `linux-image`: built and inspected, not pushed | linix | a protected ref |
| `linux-build.yml` publish | `release`, `release-sign` and `release-publish`: the `.deb`, the `.rpm` and their sums on the release | shared, linix | a `v*` tag |
| `packaging.yml` chart-and-compose | `chart-and-compose` | shared | the chart or the Compose file changed |
| `packaging.yml` update-helper | `update-helper` | shared | the helper, its test or a unit changed |
| `package-managers.yml` templates | `package-templates` | shared | the templates or their scripts changed |
| `package-managers.yml` manifests | not ported: it renders from a published, signed installer and submits it; still the release-chain row's (137) | | |
| `sbom.yml` | `linux-sbom`: made and kept; a `v*` tag's release carries them, signed | linix | a protected ref |
| `fuzz-nightly.yml` | `linux-fuzz`, on Linux rather than Windows | linix | the fuzz schedule |
| (none) | `linux-sanitize`: `snmp-tests` under AddressSanitizer, once the SNMP row adds `HM_SANITIZE` | linix | a protected ref |
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
- `verify-binary-provenance.yml`, `package-managers.yml` manifests, the image
  job's push, and the SLSA provenance and the `.sigstore.json` copies of the
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
- The AppImage. Its build script runs linuxdeploy, whose only distribution is
  GitHub's releases. It waits for the release-chain row.

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
| `linux-build`, `linux-suite`, `linux-cross`, `linux-sanitize`, `linux-db-upgrade-*`, `linux-sbom`, `linux-fuzz` | `hmailserver-ci/linux` (built on linix) | Ubuntu's packages; the .NET SDK and PowerShell from Microsoft's SDK image; Syft 1.52.0 from `anchore/syft` on Docker Hub ([The image](#the-image)) |
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
package jobs and `linux-image` fail on the missing files and point at
`linux-build/package.log`.

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
kept: the release-chain row will build and push its own.

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
2. It reverts the VM to the snapshot, starts it and waits for SSH.
3. It checks that the address answers ARP with the same MAC. This comes before
   anything is copied, so a wrong `--host` cannot install on bench150 or linix.
4. It copies the installers and `build/ci/installer-smoke.ps1` to `C:\smoke`,
   runs the script there under Windows PowerShell 5.1, and copies back
   `C:\smoke\logs`. That folder holds `smoke.log`, Inno's `install.log`,
   `upgrade.log` and `uninstall.log`, and any server logs.
5. It reverts the VM again. From the first revert to the last, a run takes
   about two minutes.

Only the SSH key is used, never a password. The administrator password given to
the installer is made on the VM and never printed. The output is one line per
check, starting PASS, FAIL, SKIP or INFO. The exit code is 0 when every check
passed, 1 when a check failed, 3 when the VM could not be identified, reverted
or reached, and 4 when the script did not run to a result.

**The switches.** The switches are the installer's own, from
`hMailServerInnoExtension.iss`. The first install gets
`/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TYPE=full /adminpassword=<password> /LOG=<file>`.
`/adminpassword=` is the only way a silent install sets the administrator
password. The upgrade needs it too: DBUpdater authenticates as the
administrator before it moves the schema, and under `/silent` it fails rather
than asks. The upgrade gets no `/TYPE`, because winget and Chocolatey pass none
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
  - the password given with `/adminpassword` authenticates;
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
  - `[Database]` and `[Security]` in `hMailServer.INI` are unchanged.
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
  when it is empty.

The database file, the message (unchanged) and every event and addon script
must still be there. Any other file left under the installation directory fails
the check.

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
| `release` | shared runners, by hand, after every gate | Refuses unless the tag is an annotated tag signed by a key in `.gitlab/allowed_signers` and still points at the commit the pipeline built, `Version.h` says the tag's version, `HM_COSIGN_IMAGE` is `gcr.io/projectsigstore/cosign` pinned by digest (checked as the job receives it, so a CI/CD variable of that name cannot put another image in `release-sign`) and the five Azure settings are set; then collects the installer, the `.deb`, the `.rpm`, their sums as `hmailserver-linux-<version>-SHA256SUMS.txt`, and the two SBOMs, checking the sums against the packages (`build/ci/release-chain.py collect`) |
| `release-authenticode` | bench150 | Signs the installer through Azure Artifact Signing and requires a valid signature with an RFC 3161 timestamp (`build/ci/windows-authenticode.ps1`) |
| `release-sign` | shared runners, in cosign's `gcr.io` image by digest | Signs all six files with Sigstore and checks each bundle's format and log entry, and verifies it with cosign (`build/ci/release-sign.sh`) |
| `release-verify` | linix | Checks every file with `hmailserver --verify-update`, built by `linux-build` from the same commit, requires the GitLab identity, refuses a copy with a byte added, and writes the notes' verification section (`build/ci/release-verify.sh`) |
| `release-publish` | shared runners, by hand | Refuses a release that exists already; uploads the twelve files to the generic package `hmailserver/<version>`, reads them back and holds each to the SHA-256 that was signed, creates the release (the version's section of `CHANGELOG.md` and the verification) with a link per file named after it, and requires the feed rendered from the API to give Windows, Debian and RPM servers their file and bundle (`build/ci/release-chain.py publish`) |
| `update-feed` | a trigger | A downstream pipeline on `master`, which runs `pages` alone: every other job refuses a pipeline whose source is `pipeline`. It passes no variables, not even the file's (`inherit: variables: false`), because GitLab takes a trigger job's variables as pipeline variables, which this project refuses at GitLab's default for a project made since 17.7 |

**Why two buttons.** Nothing is signed or sent until a maintainer starts
`release`, after every gate of the tag has passed. A manual job can be started
as soon as its own needs have finished, so `release` needs every job that can
fail the tag's pipeline, and brings the artifacts of only the three whose files
it collects. It does not wait for the scans that are `allow_failure`
(`static-analysis`, `semgrep`, `ci-image-packages`, `dependency-scan`,
`repo-hygiene`), whose failure would not hold it anyway, nor for the manual
`db-matrix` and `installer-smoke`: read the scans' findings before starting it.
`build/check-release-chain.py` fails when a job that can fail is not among its
needs. `release-publish` waits for
the second button so that the installer smoke test can run on the signed
installer first ([The installer smoke test](#the-installer-smoke-test)):
download `release-signed/` from `release-sign`'s artifacts, run the test with
the previous release and the new installer, and start `release-publish` when it
passes. Both are `allow_failure: false`, so the pipeline waits at each and the
jobs after them do not start.

**Why no secret.** Sigstore takes the job's ID token for the audience
`sigstore`; Entra ID takes another, for `api://AzureADTokenExchange`, through a
federated credential on the app registration in place of a client secret; and
the upload and the release are the job's own `CI_JOB_TOKEN`. A token lives for
the job and names the project, the tag and the pipeline.

**Why the tag's own pipeline.** Fulcio names the certificate after the ID
token's `ci_config_ref_uri`, which a child or downstream pipeline's token does
not carry, and the file's path, `.gitlab-ci.yml`, is in the identity every
server built after 6.3.3 trusts ([UpdateFeed.md](UpdateFeed.md), "What the
signing job produces"). `build/check-release-chain.py`, in the `checkers` job,
fails when a change to the file or the scripts breaks that, or the order of the
jobs, or the manual starts.

**What a release carries.** Six files and a bundle beside each: the Windows
installer, the x86-64 `.deb` and `.rpm` and their sums, and the SPDX and
CycloneDX SBOMs. No AArch64 packages, no AppImages, no SLSA provenance and no
`.sigstore.json` copies, which the GitHub releases had: linix builds x86-64
only, the AppImage tool is published only on GitHub, and the provenance
generator is GitHub's. The verification section `release-verify` writes into a
release's notes says so. The server and the tools in the installer are the
bench checkout's incremental build of the tag's commit, not a clean one, and its
OpenSSL, libpq and C++ runtime come from the bench's library tree and Visual
Studio; the notes say that too.

**The artifacts' lifetimes.** `linux-build`'s artifact is kept three days, so
`release` has to be started within three days of the tag's pipeline, or
`linux-build` retried first. `release` keeps what the later jobs need, the
server binary included, for thirty days, and the signed files are kept ninety.

### What waits for the owner

Each is a one-time step. The first is done. Until either of the next two is
done, the job that needs it stops with a sentence naming it, and nothing is
signed. The fourth is
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
   administrator ([bench150](#bench150)).
3. **The Azure side of Authenticode**: the federated credential and the five
   CI/CD variables (RELEASE.md, "Authenticode", has what to create).
4. **The package registry**: on for the project (Settings, General,
   Visibility, Packages). `release-publish` uploads and creates the release
   with `CI_JOB_TOKEN`, which acts with the permissions of whoever started the
   job, so it is started by a Maintainer.
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
  does not rely on: it reads the registry back instead.

Dependency updates
------------------

Renovate proposes the project's dependency updates as merge requests on GitLab,
the work Dependabot did on GitHub. Dependabot's updater comes from GitHub's own
registry, and this project takes nothing from GitHub. Renovate is open source, its
image is on Docker Hub (`renovate/renovate`, pinned by digest in the CI file), and
it supports GitLab itself. `renovate.json` at the repository root is its
configuration. The `renovate` job runs it on linix, in a scheduled pipeline and in
nothing else.

**It has not run on GitLab yet.** The job needs a token, a CI variable and a
schedule that only the owner can create ([The owner's one-time
steps](#the-owners-one-time-steps)). Until they exist, GitLab creates no
`renovate` job. What it will propose is known from the dry runs of 25 September
2026 ([Reading a dry run](#reading-a-dry-run)).

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
(`registry.gitlab.com/progressiverobot/hmailserver`, which `linux-image` builds
and does not push: the project's container registry is not switched on), and
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

**No job has run on either runner yet.** A time marked *by hand* is the job's
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
| `linux-suite` | 1.5 to 2.5 h | 3 h | The whole suite took 75 to 90 minutes on GitHub's hosted runner on 16 September 2026, and it has grown since. Add about 5 minutes for the layout and the restore |
| `linux-cross` | 8 min by hand | 1 h | 750 translation units compiled for AArch64 with six jobs |
| `linux-sanitize` | seconds by hand, while it skips | 1 h 30 min | Once `HM_SANITIZE` exists, an estimated 15 to 25 minutes: the core under AddressSanitizer, RelWithDebInfo |
| `linux-db-upgrade-pgsql`, `-mysql` | 6 s and 21 s by hand | 20 min | 69 upgrade steps from 6029 |
| `linux-package-deb`, `-rpm` | 17 s and 2 s by hand | 20 and 10 min | |
| `linux-image` | 18 s by hand | 30 min | kaniko over Ubuntu 24.04 and the `.deb` |
| `linux-sbom` | 6 s by hand | 15 min | |
| `windows-gate` | 2.5 to 3 h | 4 h | On 25 September 2026 the two halves took 62 minutes (1,339 tests, on bench150) and 70 minutes (2,126 tests, on the laptop), each with its build. One bench runs both, one after the other, with one build. Add any wait for the lock |
| `windows-dotnet` | 10 to 20 min, estimated | 4 h | Two restores (cold for LocalSystem the first time), two Release builds, MIDL, the tests and five publishes; the timeout allows 200 minutes of waiting for the lock |
| `linux-fuzz` | 65 min | 2 h | Three harnesses of 20 minutes, plus about 3 minutes to build and replay (by hand: 3.5 minutes all told with one minute a harness) |
| `static-analysis` | 35 to 50 min | 90 min | Its script, run by hand on linix on 25 September 2026 in the job's image with four CPUs where the job has six, beside other work: 51 min 28 s, of which cppcheck took 2 min 22 s and clang-tidy 49 min for 741 files |
| `semgrep` | 3 to 5 min | 30 min | The same, with one CPU: 2 min 36 s to 2 min 50 s, the install and the rules' download included |
| `ci-image-packages` | under a minute | 10 min | The copy takes a second; the rest is the job's start |
| `dependency-scan` | 1 to 2 min | 30 min | The same, with one CPU: 15 to 17 s, the vulnerability database's download included |
| `repo-hygiene` | 1 min | 10 min | The same, with one CPU: 14 to 37 s, the apt install included |
| `semgrep-sast`, `secret_detection` | not measured | 30 and 30 min | Neither has run on GitLab. Each counts against the namespace's compute minutes on GitLab's shared runners. `secret_detection`'s scan of the whole history took under a minute on linix |
| `renovate` | 2 to 5 min | 1 h | The dry runs of 25 September 2026 on linix: 5 s for the advisory database, 9 s to install the .NET SDK, 1 to 1.5 min for Renovate. The first run also pulls its image, about 450 MB |

The runners' own `maximum_timeout` is four hours. GitLab lets a job's
`timeout:` exceed the project's default of one hour, but not exceed its
runner's maximum. linix runs one job at a time, so its jobs add up: about 12
minutes of build, 1.5 to 2.5 hours of suite and about 10 minutes of the rest, so
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
serves it. GitLab Pages deploys `public/` when the job passes. The script reads
GitLab's public API and needs no token and no variable. What it publishes is
what the releases say at that moment, whichever commit ran it.

It runs for a push to `master`, for a `v*` tag, and for a pipeline schedule on
`master` (the feed's own is described `update feed`) but the three described
`fuzz`, `renovate` and `security`, and never for a merge request, a `batch*`
branch or any other branch.
A release is created on GitLab after its tag is pushed, so the tag's own run
of `pages` misses it. A release made by the release jobs ends with
`update-feed`, which starts a pipeline on `master` running `pages` alone; the
schedule brings the feed up to date after a release made or edited any other
way.
Until there is one, running a pipeline on `master` by hand after a release is
published does the same (Build, Pipelines, Run pipeline), along with every other
`master` job.

**A schedule's pipeline runs `pages` alone, and so does the feed's.** A
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
findings are triaged, so none of them can fail a pipeline yet.

Two more come from GitLab's own templates (`.gitlab/ci/gitlab-security.yml`,
included from `.gitlab-ci.yml`): `semgrep-sast`, GitLab's Semgrep analyzer and
rules over every language of the tree, the server's C and C++ included, and
`secret_detection`, which looks for a key, token or password in the commits a
pipeline brings. They run on GitLab's shared runners, for `master` and a merge
request only, in images from `registry.gitlab.com` pinned by digest. Each
leaves its JSON report for Developers and above to download.
`SAST_DISABLED=true` or `SECRET_DETECTION_DISABLED=true`, as a CI/CD variable,
turns one off.

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
testers, AWS's documented example key `AKIAIOSFODNN7EXAMPLE` in
`ObjectStoreSignature.cpp`'s tester, the regression tests' own fixture keys,
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
| `static-analysis` | the CI image | cppcheck 2.19.0 and clang-tidy 21.1.8, from Ubuntu 26.04's archive, installed as the job starts. clang-tidy is the image's own clang 21 | `gl-sast-cppcheck.json`, `gl-sast-clang-tidy.json`, `junit.xml` | 51 min 28 s on 4 CPUs |
| `semgrep` | `python:3.13-slim-trixie`, pinned by digest (Docker Hub) | Semgrep 1.178.0 and 65 packages it needs, from PyPI, with every file's SHA-256 pinned (`build/ci/requirements.txt`, `pip --require-hashes`). The rules come from Semgrep's registry at semgrep.dev (`p/csharp`, `p/python`, `p/javascript`, `r/bash`), fetched with no account and no token | `gl-sast-semgrep.json`, `junit.xml` | under 3 min on 1 CPU |
| `ci-image-packages` | the CI image | none | the image's `etc/os-release`, `etc/lsb-release`, dpkg status and the .NET SDK's `deps.json` files, 1.4 MB | seconds |
| `dependency-scan` | `aquasec/trivy:0.74.0`, pinned by digest (Docker Hub) | Trivy 0.74.0; its vulnerability database from `mirror.gcr.io`, with `public.ecr.aws` as the fallback; OSV's API at `api.osv.dev`; Python from Alpine's repository | `gl-dependency-scanning-trivy.json`, `gl-dependency-scanning-osv.json`, `junit.xml` | 15 to 17 s, the database download included |
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
  reports, and are also in its ordinary artifacts (`security/`). On the free
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
   `# nosemgrep`.
4. If it is real but not to be fixed now, regenerate the baseline in a commit
   that says why.

To regenerate a baseline, run the job's script by hand on linix with
`--write-baseline`, from a checkout of the commit:

    docker run --rm -v "$PWD":/src -w /src hmailserver-ci/linux:<tag> bash build/ci/static-analysis.sh --write-baseline
    docker run --rm -v "$PWD":/src -w /src python:3.13-slim-trixie bash build/ci/semgrep.sh --write-baseline

Or take a job's artifacts, which hold the tools' raw output, and run
`build/ci/security-findings.py --write-baseline` over them. Its help gives the
command, and nothing is scanned again. A line may be deleted from a baseline by
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
  suite's own PostgreSQL service. The project's job-token allowlist and who may
  override pipeline variables are not public. A maintainer's `glab api
  projects/86730559` read `ci_job_token_scope_enabled: false` (the allowlist,
  "Limit access to this project", is off) and
  `ci_pipeline_variables_minimum_override_role: no_one_allowed`.

Left out of Scorecard's checks, each for a reason:

- **Code-Review.** The maintainers commit to `master` without merge requests,
  which is the project's rule, and GitLab's approval rules are a paid feature.
- **Maintained** and **Contributors.** They measure activity and outside
  organisations, not the repository's settings.
- **CII-Best-Practices.** It looks up an OpenSSF Best Practices badge, which
  the project does not hold.
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
  (`bash build/linux-tu-census.sh --cc g++` is the GCC census), and the AppImage.
- The Windows ARM64 build: the ARM64 libraries, `build.ps1 -Platform ARM64`,
  `build-tools.ps1 -Platform ARM64` and `hMailServerARM64.iss`. No job builds it:
  bench150 is x64 and has no ARM64 compilers, and no runner is an ARM64 Windows
  machine to test it on ([WindowsOnArm.md](WindowsOnArm.md) has the commands).
- The release's two buttons, `release` and `release-publish`
  ([The release](#the-release)), and pushing the image and the package
  managers' manifests, which are still the Roadmap2 row "The release chain off
  a single forge".
- The installer smoke test, which runs from the VMware host with
  `build/ci/installer-smoke-run.sh`. A batch runs it before it lands, and a
  release runs it before it is published (see [The installer smoke
  test](#the-installer-smoke-test)).
- The builds and both suites for any branch that is not protected. A
  contributor runs them before opening a merge request, and the maintainer runs
  them before a change lands, as [CONTRIBUTING.md](../../CONTRIBUTING.md)
  describes.
- `check-package-manifests.py`, the PowerShell checks (`check-schema-versions.ps1`,
  `check-db-scripts.ps1`, `check-roadmap.ps1`, `generate-settings-index.ps1`),
  editorconfig-checker and `dotnet format`.
- The Linux build under GCC, the AArch64 build and the cross-compile census.
  GitHub's `linux-build.yml` ran them, and linix is one x86-64 machine.
  `build/linux-tu-census.sh` runs the census by hand.
- The schema-upgrade gate (`build/db-upgrade-gate.sh`), the Control Panel's
  tests and the tools' tests.
- The AppImage, the AArch64 packages, pushing the container image and the
  package managers' manifests: the Roadmap2 row "The release chain off a single
  forge". Signing and publishing the rest are the release jobs
  ([The release](#the-release)).
- Building the Linux job image when its Dockerfile changes (see [The
  image](#the-image)), and creating the fuzz schedule once (see [The fuzz
  schedule](#the-fuzz-schedule)).
- Bringing a runner back when its machine has been rebuilt (see
  [Re-creating a runner](#re-creating-a-runner)).
