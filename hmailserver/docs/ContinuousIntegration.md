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
| `linux-fuzz` | test | linix | a pipeline with `HM_PIPELINE=fuzz`, from its schedule | The three MIME harnesses under libFuzzer (`build/ci/linux-fuzz.sh`) |
| `release` | release | GitLab's shared runners | a `v*` tag, started by hand | A placeholder. It fails with "the release stage is the release-chain row's (137)" |
| `installer-smoke` | release | GitLab's shared runners | a `batch*` branch or a `v*` tag, started by hand | A placeholder. It fails with a sentence saying that the installer smoke test runs from the VMware host ([The installer smoke test](#the-installer-smoke-test)) |

`linux-build`, `windows-gate`, `windows-dotnet`, `linux-cross`, `linux-sanitize`
and `linux-sbom` start as soon as the pipeline does. They do not wait for the
checks, because the two machines are otherwise idle and the Windows gate is the
longest job. `linux-suite`, the two upgrade gates, the two package jobs and
`linux-image` wait for `linux-build`, whose artifact holds the binary and the
packages, and `release` waits for the checks and the two suites. linix runs one
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
| `master` | the branch's | all of them but `release`, `linux-fuzz` and `installer-smoke` |
| a `batch*` branch | the branch's | all of them but `release`, `linux-fuzz` and `pages`, with `installer-smoke` manual: this is the gate a batch passes before it lands |
| any other branch, no open merge request | the branch's | `checkers`, `harnesses`, `editorconfig`, and the four path-bound checks whose files changed |
| any other branch, with an open merge request | the merge request's | `checkers`, `harnesses`, `editorconfig`, the path-bound checks whose files the merge request changes, and `sign-off` when the branch is in a fork |
| a merge request from `master` or a `batch*` branch | none; the branch's own pipeline stands for it | as for the branch |
| a `v*` tag | the tag's | all of them but `linux-fuzz`, with `release` and `installer-smoke` manual |
| any other tag | none | |
| the fuzz schedule (`HM_PIPELINE=fuzz`, on `master`) | the schedule's | `linux-fuzz` alone |
| any other pipeline schedule on `master` | the schedule's | `pages` alone (see [The update feed](#the-update-feed)), or the one job the schedule's variable names |

The four path-bound checks - `chart-and-compose`, `update-helper`,
`package-templates` and `roadmap` - run when a file they check, or
`.gitlab-ci.yml`, changed, as their GitHub workflows did (`rules:changes`). For
a branch's first push and for a tag GitLab counts every file as changed, so they
all run then.

The `workflow:` rules keep this to one pipeline per change. A branch with an
open merge request would otherwise run two pipelines, one for the branch and
one for the merge request. A newer push to the same ref makes an older pipeline
moot: every job is `interruptible`, so GitLab cancels the older one when the
project's "auto-cancel redundant pipelines" setting is on, as it is by default.
`linux-fuzz` is the exception: it is not interruptible, so a push to `master`
during the night does not cut the night's fuzzing short.

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

The job itself adds three exit codes: 10 when the lock stayed another holder's
for `HM_GATE_LOCK_WAIT_MINUTES` (90), 11 when the commit could not be fetched,
and 12 when the commit has no `build/ci`.

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
  gives up with exit 10.

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

Every job of the old `.github/workflows`, and where it went. The workflows stay
in the tree in case GitHub returns; GitLab reads none of them.

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
| `linux-build.yml` publish | not ported: the release-chain row (137) | | |
| `packaging.yml` chart-and-compose | `chart-and-compose` | shared | the chart or the Compose file changed |
| `packaging.yml` update-helper | `update-helper` | shared | the helper, its test or a unit changed |
| `package-managers.yml` templates | `package-templates` | shared | the templates or their scripts changed |
| `package-managers.yml` manifests | not ported: it renders from a published, signed installer and submits it; the release-chain row (137) | | |
| `sbom.yml` | `linux-sbom`: made and kept, not attached | linix | a protected ref |
| `fuzz-nightly.yml` | `linux-fuzz`, on Linux rather than Windows | linix | the fuzz schedule |
| (none) | `linux-sanitize`: `snmp-tests` under AddressSanitizer, once the SNMP row adds `HM_SANITIZE` | linix | a protected ref |
| `codeql.yml` | not ported: see below | | |
| `dependency-review.yml` | not ported: see below | | |
| `scorecard.yml` | not ported: see below | | |
| `upstream-watch.yml` | not ported: see below | | |
| `sign-release.yml` | not ported: the release-chain row (137) | | |
| `verify-binary-provenance.yml` | not ported: the release-chain row (137) | | |
| `installer-smoke.yml` | ported separately, by its own change | | |

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
- `sign-release.yml`, `verify-binary-provenance.yml`, `linux-build.yml`
  publish, `package-managers.yml` manifests, and the upload halves of
  `sbom.yml` and the image job. Signing, publishing, the update feed and what a
  release attaches are the Roadmap2 row "The release chain off a single forge"
  (137). The jobs here make and check the packages, the image and the SBOMs;
  that row decides where they go.
- `installer-smoke.yml`. Another change ports it.
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
night to the next. A finding fails the job and keeps the input in its artifact
for 30 days; [Fuzzing.md](Fuzzing.md) says what to do with one.

It runs in a pipeline of its own and nowhere else: its rule asks for
`HM_PIPELINE` to be `fuzz` on a protected ref, and every other job refuses a
pipeline where it is. **Creating the schedule is a one-time step for the owner**,
in the project's *Build*, *Pipeline schedules*, *New schedule*:

- description `Nightly fuzz`; interval pattern `10 2 * * *` (custom), time zone
  UTC - 02:10, after the day's pushes, as before;
- target branch `master`;
- one variable, `HM_PIPELINE` = `fuzz`; and, to change the time box,
  `HM_FUZZ_MINUTES` (three harnesses run in turn: keep it at 35 or less inside
  the job's two hours) and `HM_FUZZ_JOBS`;
- activated.

The schedule runs as the maintainer who owns it, who must be allowed to push to
`master`, because a scheduled pipeline on a protected branch is that user's.
The same pipeline can be started by hand from *Run pipeline* on `master` with
`HM_PIPELINE=fuzz`. It occupies linix for a little over an hour, and a
protected ref's pipeline that arrives meanwhile waits for it. To start the
corpus again from the seeds, clear the runner caches (*Pipelines*, *Clear
runner caches*).

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
GitHub's `.github/workflows/installer-smoke.yml`, which ran on a hosted runner
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
that host.

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

The runners' own `maximum_timeout` is four hours. GitLab lets a job's
`timeout:` exceed the project's default of one hour, but not exceed its
runner's maximum. linix runs one job at a time, so its jobs add up: about 12
minutes of build, 1.5 to 2.5 hours of suite and about 10 minutes of the rest, so
three hours or a little more. bench150's two jobs add up the same way. So a
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
`master`, and never for a merge request, a `batch*` branch or any other branch.
A release is created on GitLab after its tag is pushed, so the tag's own run
can miss it. The schedule is what brings the feed up to date without a push.
Until there is one, running a pipeline on `master` by hand after a release is
published does the same (Build, Pipelines, Run pipeline), along with every other
`master` job.

**A schedule's pipeline runs `pages` alone.** A scheduled pipeline is a
pipeline on `master` to every job in the file, so each of the others says it is
not for one: `.own-runners` (and so every job on linix and bench150) and
`.not-a-fuzz-pipeline` (and so `checkers`, `harnesses` and `editorconfig`)
begin with `- if: $HM_PIPELINE == "fuzz" || $CI_PIPELINE_SOURCE == "schedule"`
with `when: never`, and the four path-bound checks carry the same rule. `pages`
itself skips the fuzz pipeline. Without them each scheduled run would start the whole Windows gate
on bench150 and the Linux suite on linix. A new job added to the file needs the
same, or a schedule starts it too.

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
- Signing, publishing the packages, the image and the SBOMs, the release's
  manifests, and the update feed. These belong to the Roadmap2 row "The release
  chain off a single forge". The `release` stage is its placeholder.
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
- Signing, the `.deb`, `.rpm` and AppImage packages, the container image and
  publishing; the update feed is the `pages` job's. These belong to the
  Roadmap2 row "The release chain off a single forge". The `release` stage is
  its placeholder.
- Building the Linux job image when its Dockerfile changes (see [The
  image](#the-image)), and creating the fuzz schedule once (see [The fuzz
  schedule](#the-fuzz-schedule)).
- Bringing a runner back when its machine has been rebuilt (see
  [Re-creating a runner](#re-creating-a-runner)).
