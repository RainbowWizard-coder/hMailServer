Continuous integration
======================

GitLab CI starts every pipeline (`.gitlab-ci.yml` at the repository root). The
cheap checks run on GitLab's shared runners, for every push and every merge
request. The builds and both regression suites run on the project's own two
machines, and only for the refs that only a maintainer can push. The owner
decided this on 25 September 2026, a week after GitHub suspended the project's
account and its workflows with it.

**Nothing here has run yet.** When this was written, neither runner had taken a
job. Every duration below is an estimate made from the gates run by hand, and it
says so. Replace the estimates with the first runs' times.

What runs where
---------------

| Job | Stage | Runs on | Runs for | What it does |
|---|---|---|---|---|
| `checkers` | check | GitLab's shared runners | every pipeline | Every Python checker, the self-tests, the generators' `--check` ([CONTRIBUTING.md](../../CONTRIBUTING.md), "The checkers") |
| `harnesses` | check | GitLab's shared runners | every pipeline | The Control Deck's and the webmail's script harnesses, and the S/MIME and OpenPGP checks |
| `sign-off` | check | GitLab's shared runners | a merge request from a fork | Every commit carries its author's `Signed-off-by` |
| `linux-build` | build | linix | a protected ref | The Linux server with clang, every `*-tests` program the CMake build makes, and the OpenPGP writer against GnuPG (`build/ci/linux-build.sh`) |
| `linux-suite` | test | linix | a protected ref | The whole of `LinuxRegressionTests` against that server on PostgreSQL 16 (`build/ci/linux-suite.sh`) |
| `windows-gate` | test | bench150 | a protected ref | The whole Windows suite in the bench's own checkout (`build/ci/windows-gate.ps1`) |
| `release` | release | GitLab's shared runners | a `v*` tag, started by hand | A placeholder. It fails with "the release stage is the release-chain row's (137)" |

`linux-build` and `windows-gate` start as soon as the pipeline does. They do not
wait for the checks, because the two machines are otherwise idle and the
Windows gate is the longest job. `linux-suite` waits for `linux-build`, and
`release` waits for everything else.

A test report is published for both suites, under the pipeline's *Tests* tab.
The Linux suite's TRX goes through `build/ci/trx-to-junit.py`, and the Windows
suite's NUnit result goes through `build/ci/nunit-to-junit.ps1`. Skipped tests
are expected and do not fail a job: a Linux fixture that the REST API cannot
drive says why and skips, and a Windows test skips where the bench lacks what
it needs (a real `python.exe`, for one). A job passes on "Failed 0" with at least one
test passed. A failed test fails it, and so does a run of no tests.

Which pipeline runs what
------------------------

| Push or event | Pipeline | Jobs |
|---|---|---|
| `master` | the branch's | all of them but `release` |
| a `batch*` branch | the branch's | all of them but `release`: this is the gate a batch passes before it lands |
| any other branch, no open merge request | the branch's | `checkers`, `harnesses` |
| any other branch, with an open merge request | the merge request's | `checkers`, `harnesses`, and `sign-off` when the branch is in a fork |
| a merge request from `master` or a `batch*` branch | none; the branch's own pipeline stands for it | as for the branch |
| a `v*` tag | the tag's | all of them, and the manual `release` |
| any other tag | none | |

The `workflow:` rules keep this to one pipeline per change. A branch with an
open merge request would otherwise run two pipelines, one for the branch and
one for the merge request. A newer push to the same ref makes an older pipeline
moot: every job is `interruptible`, so GitLab cancels the older one when the
project's "auto-cancel redundant pipelines" setting is on, as it is by default.

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
3. the job's `rules:` created it. `.own-runners` in the CI file creates the
   three jobs only when `$CI_COMMIT_REF_PROTECTED` is `"true"` and the pipeline
   is not a merge request's.

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

Nothing else is installed on the host for CI. The toolchain, the .NET SDK and
the PostgreSQL client are in the image.

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

`linux-build` and `linux-suite` run in one image, described by
`build/ci/linux.Dockerfile`. The image holds:

- Ubuntu 26.04 at a pinned digest;
- the toolchain and the `-dev` packages of the Linux build;
- GnuPG, the PostgreSQL client, curl and Python;
- the .NET 10 SDK, copied from Microsoft's SDK image at a pinned digest.
  Ubuntu's own `dotnet-sdk-10.0` is a 10.0.1xx SDK, and `global.json` refuses
  it.

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
describes.

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

How long each job takes
-----------------------

**These are estimates.** No job has run on either runner yet. Each estimate is
reasoned from the gates run by hand. Replace them with the measured times after
the first pipelines.

| Job | Estimate | Timeout | Where the estimate comes from |
|---|---|---|---|
| `checkers` | 4 to 6 min | 20 min | Row 139 estimated about six minutes for the checks together, from their run time on a laptop |
| `harnesses` | 2 to 3 min | 20 min | The same estimate |
| `linux-build` | 15 to 30 min | 1 h | A clean clang Release build of the core (about 500 translation units) with six jobs, plus the testers, which take seconds |
| `linux-suite` | 1.5 to 2.5 h | 3 h | The whole suite took 75 to 90 minutes on GitHub's hosted runner on 16 September 2026, and it has grown since. Add about 5 minutes for the layout and the restore |
| `windows-gate` | 2.5 to 3 h | 4 h | On 25 September 2026 the two halves took 62 minutes (1,339 tests, on bench150) and 70 minutes (2,126 tests, on the laptop), each with its build. One bench runs both, one after the other, with one build. Add any wait for the lock |

The runners' own `maximum_timeout` is four hours. GitLab lets a job's
`timeout:` exceed the project's default of one hour, but not exceed its
runner's maximum. So a pipeline for a protected ref takes about three hours
from start to finish, and the Windows gate decides how long.

What each job leaves behind
---------------------------

- **`linux-build`** builds in its container's own filesystem (`/tmp/hm-build`),
  which goes when the container does. The runner's build volume on linix keeps
  the checkout and `linux-build/`, the server binary, its hash and the build
  log, until the next job's checkout cleans it. The artifact expires after
  three days.
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
  `postgres:16`, the runner's helper image and its build and cache volumes.
- **`windows-gate`** leaves the bench as the split gate leaves it: the bench's
  checkout at the gated commit, the service running that build, `C:\HMTest` as
  the suite left it, and the fetched objects in the checkout's `.git`. The
  job's directory under `C:\GitLab-Runner\builds` keeps `gate\`, `gate.zip` and
  `gate-results\`, a few megabytes that the next job replaces.
  `C:\bench\gate.lock` exists only while a gate holds it. The artifact expires
  after seven days.

What is still done by hand
--------------------------

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
- Signing, the `.deb`, `.rpm` and AppImage packages, the container image,
  publishing and the update feed. These belong to the Roadmap2 row "The release
  chain off a single forge". The `release` stage is its placeholder.
- Building the Linux job image when its Dockerfile changes (see [The
  image](#the-image)).
- Bringing a runner back when its machine has been rebuilt (see
  [Re-creating a runner](#re-creating-a-runner)).
