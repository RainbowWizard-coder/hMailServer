# Release checklist

The order below is not advisory. Each rule exists because violating it has
already cost a release cycle or nearly shipped a defect.

**Status since 18 September 2026.** GitHub has been unavailable to the project
since 18 September 2026, and its home has been
[GitLab](https://gitlab.com/Progressiverobot/hmailserver) since 22 September
2026. Steps 11 to 13 below describe the release chain as it ran on GitHub -
the draft release, the Linux packages, the SBOMs, Sigstore and Authenticode
signing, the installer smoke test and the package managers as GitHub Releases
and GitHub Actions - and are kept as the history of how releases up to 6.3.3
were made. From 6.3.5 a release is made by the tag's GitLab pipeline instead:
[The release on GitLab](#the-release-on-gitlab), below, replaces steps 11 and 12
from the tag on. 6.3.4 was built and Authenticode-signed by hand, between the
two, and carries no Sigstore bundle; its sums file is signed with the release
tag's SSH key instead ([A release made by hand](#a-release-made-by-hand)). The bench steps run as written; step 9
reads the scan reports of the commit's GitLab pipeline where it read GitHub's
Code Scanning and Dependabot pages up to 6.3.3. Step 12's commit rule is out of
date as well: since 24 September 2026 commits are made as Christopher
Holloway, the identity of the GitLab account `Progressiverobot`; from 14
September 2026 up to and including 6.3.4 no approval was required, so a tree that
had passed the gate landed on `master` by fast-forward, with no pull request;
and from the first change after 6.3.4 every change, a release's included, lands
through a merge request that the other maintainer approves before the owner
merges it by fast-forward, its pipeline green (GOVERNANCE.md, Review). A batch
lands that way with `python3 build/land-via-mr.py batchN` (written on 26
September 2026; none has landed yet), which refuses a merge request without that
approval unless the owner overrides it for one landing (`--land-without-approval
"<reason>"`), and every commit it lands is signed off (`git commit -s`;
CONTRIBUTING.md, "Merge requests"). Until the first such landing, a tree that has
passed the gate is pushed to `master` directly.

**From 6.3.6, releases pause.** 6.3.6 is the last release for the time being.
Security releases are still made, until 30 September 2027
(SECURITY.md, "While releases pause"), in the way
[A security release during the pause](#a-security-release-during-the-pause)
describes below.

## The release on GitLab

For 6.3.5 onwards. Written on 26 September 2026 and not yet run: the first
release made this way is also the first test of it, and
`hmailserver/docs/ContinuousIntegration.md` ("The release") lists what only that
run can show and the one-time steps the owner takes first. The jobs are in
`.gitlab-ci.yml`, and `build/check-release-chain.py` holds them to what a server
built after 6.3.3 needs.

Before the first release, and whenever the project's members or settings
change, check the protected tag rule (Settings, Repository, Protected tags):
`v*`, allowed to create: Maintainers, and nobody else. Every server built after
6.3.3 trusts a bundle signed by any pipeline of a `v` tag of this project,
whatever `.gitlab-ci.yml` that tag's commit has, so whoever can push such a tag
can sign a release the servers accept. Nothing in the pipeline can check this.

Release tags are created by Christopher Holloway, as `Progressiverobot`, alone
(the owner's decision of 26 September 2026, GOVERNANCE.md). The protected tag
rule cannot say so on GitLab's free tier - it names a role, and Maintainers
include others - so the `release` job holds what it can: it reads
`.gitlab/allowed_signers` as `master` has it, fetched by name, never as the
tag's own tree has it; it refuses every tag while that list holds any number of
keys but one; it requires the tag's signature to be that key's, by its
fingerprint; and it requires the tagged commit to be on `master`. These checks
run as the tagged commit has them, so a tag on a commit that is not `master`'s
could carry a `.gitlab-ci.yml` or a `build/ci/release-chain.py` of its own
without them: they stop a mistake and a tag pushed without the owner's key.
Against a hostile commit, the protected tag rule is what holds for the Sigstore
signature every server checks: any pipeline of a protected `v*` tag is given a
certificate naming that tag, which servers accept. The Azure credential made
for one tag at a time ([Authenticode](#on-gitlab-from-635)) holds the
installer's Authenticode signature alone, which a server checks only where
`UpdateRequireAuthenticode=1` is set, and never on Linux. `master`'s list is
itself only as safe as `master`, which Maintainers may push to until its push
rule becomes *No one*; after that a change to it reaches `master` only through a
merge request whose pipeline passed (`hmailserver/docs/ContinuousIntegration.md`,
"Landing on master").

1. **Steps 1 to 10 below, as written**, with one change: the release notes are
   the version's section of `CHANGELOG.md`, headed `## [X.Y.Z](...) - <date>`,
   committed before the tag, because `release-publish` makes the release's notes
   from it as the tag's own tree has it and refuses a version without one - a
   pre-release's too, `## [X.Y.Z-rc1](...)`. The installer is not built by hand
   (step 11): the pipeline builds it, in bench150's checkout, and refuses to if
   that checkout has a change of its own to a tracked file, or a file git does
   not track where a build would take it in (`build/ci/windows-installer.ps1`
   says which).
2. **Tag the release on `master`**, annotated and signed with the owner's key,
   once the commit is on GitLab's `master` - where the release commit, with the
   version stamp and the notes, gets through its batch's merge request
   (`python3 build/land-via-mr.py batchN`), signed off like every other - and
   push the tag:

   ```
   git tag -s vX.Y.Z -m "hMailServer X.Y.Z"
   git -c gpg.ssh.allowedSignersFile=.gitlab/allowed_signers verify-tag vX.Y.Z
   git push gitlab vX.Y.Z
   ```

   `release` refuses a tag whose commit GitLab's `master` does not have yet, and
   reads the key from `master`'s `.gitlab/allowed_signers`, so a key changed in
   the tagged commit alone counts for nothing.

   A pre-release's tag carries a suffix, `vX.Y.Z-rc1` (step 12 says why).
   The tag's pipeline runs every gate - the checks, both suites, the package
   jobs, the SBOMs - and `windows-installer`, which builds the installer on
   bench150 after `windows-gate` and signs nothing.
3. **Start `release`** once every job before it has passed, within three days of
   the tag's pipeline (`linux-build`'s artifact is kept three days; retry
   `linux-build` otherwise). GitLab offers the button only when every job of
   `.gitlab-ci.yml` that can fail the pipeline has passed and `static-analysis`,
   `semgrep`, `dependency-scan` and `semgrep-sast` have finished, whatever they
   found; it does not wait for `ci-image-packages` or `repo-hygiene`, which are
   `allow_failure`, or for the manual `db-matrix`. Of the package jobs of
   `.gitlab/ci/packages-*.yml` it waits for the two whose generic files the
   release carries - `deb-ubuntu26.04-arm64`, the generic arm64 `.deb`, and
   `rpm-fedora44-aarch64`, the generic aarch64 `.rpm` - with their SBOM jobs and
   their install jobs, which install and run each package in an AArch64
   container under qemu-user; for `release-packages`
   (`.gitlab/ci/release-packages.yml`), which waits in turn for every other
   distribution release's `.deb` and `.rpm` build and its install job, both
   AppImages and their test jobs, `appimage-runtime-sources` (the source of the
   libraries linked into the AppImages' runtime), `appimage-bundled-sources` (the
   source of the libraries they carry in `usr/lib`) and the source tarball, and
   gathers those files, each but the two sources files with an SBOM; and for
   `helm-chart-package`, the chart. (Release may need fifty jobs at most,
   GitLab's `ci_needs_size_limit`; `release-packages` waits for forty-three,
   forty-one of them jobs `release` does not need itself.) It does not wait for the Windows packages, whose installers no
   release carries: the release's installer is `windows-installer`'s. It reads
   the four scans'
   reports first (`build/ci/security-gate.py`) and refuses while one did not
   finish, or found something `SECURITY.md`'s policy counts ("Before a
   release") that no baseline and no entry of `.gitlab/security-exceptions.yml`
   accepts. Fix the finding, which takes a new tag on the fixed commit, or have
   the owner accept it in that file on `master`, with a reason and an expiry,
   and retry `release`: the job reads `master`'s copy of the file, never the
   tagged tree's. It refuses, naming why, a tag that is lightweight, moved to
   another commit since the pipeline started, on a commit `master` does not
   have, or not signed by the one key `master`'s `.gitlab/allowed_signers`
   lists (and every tag while that list holds more than one); a `Version.h` that
   does not say the tag's version, a cosign image that is not `gcr.io`'s pinned
   by digest, a missing Azure setting, a missing file, a file unlike the sums
   its build job wrote, a compiled file without an SBOM that describes it, and a
   source tarball `build/check-source-tarball.py` does not take for this commit.
   It also refuses when
   the package `hmailserver/X.Y.Z` already holds one of the release's files with
   other bytes, or the installer or a bundle at all: those two are made by the
   signing jobs after it, and a second signing never makes the same bytes (a new
   timestamp, a new certificate). And it counts what GitLab and Pages will take
   before anything is signed: every link of the release GitLab must accept (a
   `direct_asset_path` of at most 128 characters of letters, digits, `_ . - /`),
   and the update feed's site with the release in it - `_redirects`, with the apt
   and dnf repositories' rules, under the 1,000 rules and 64 KiB Pages reads, and
   `releases.json` under the 4 MiB a server reads, the repositories kept as
   `package-repos`' `--keep` keeps them - and says the counts. Nothing
   has been signed or sent when it refuses.

   Before starting it, compare the line `hMailServer.exe <SHA-256>` that
   `windows-installer` printed with the hash step 8 measured and the notes
   give. The installer carries that executable, built in bench150's checkout;
   a different hash means bench150's compiler, Windows SDK or libraries are
   not the ones step 8 used, and the notes would name an executable the
   release does not ship. Bring bench150 to that toolchain and retry
   `windows-installer` first ([ReproducibleBuilds.md](hmailserver/docs/ReproducibleBuilds.md)).
4. **`release-authenticode`, `release-sbom`, `release-sign` and `release-verify`
   follow by themselves.** `release-sbom` makes the signed installer's own SBOM
   and the statement of the release's SLSA v1 provenance, which names every file
   by its SHA-256; the x86-64 `.deb`'s and `.rpm`'s SBOMs come from `linux-build`,
   the generic AArch64 ones' from `deb-ubuntu26.04-arm64-sbom` and
   `rpm-fedora44-aarch64-sbom`, and every other package's, both AppImages' and the
   tarball's from `release-packages`, and every compiled file goes out with its
   `<file>.cdx.json` (SECURITY.md, "Software bills of materials"). From 6.3.6 a
   release is fifty-eight files, each with its bundle: the installer; the generic
   `.deb` and `.rpm` for x86-64 and for AArch64; a `.deb` for Debian 12 and 13 and
   Ubuntu 24.04 and an `.rpm` for Fedora 44 and 43, EL 10 and 9, openSUSE Leap 16.0
   and Tumbleweed, each for both, eighteen; the two AppImages; the source of the
   libraries linked into their runtime, `hmailserver-X.Y.Z-appimage-runtime-sources.tar.gz`,
   which the LGPL-2.1 and the GPL of two of them ask for and each AppImage names
   (hmailserver/docs/PackagesAppImage.md, "Licences"); the source of the libraries
   they carry in `usr/lib`, `hmailserver-X.Y.Z-appimage-bundled-sources.tar.gz`,
   Ubuntu's source packages, which the LGPL of GnuTLS and six others asks for and
   each AppImage names in `bundled.txt`; the source tarball; the Helm chart; the
   sums of every Linux file, the two sources files and the chart;
   the twenty-six SBOMs of the compiled files and the tarball, and the tree's two.
   Each primary file's bundle goes out a second time as `<file>.sigstore.json`,
   and `hmailserver-X.Y.Z.intoto.sigstore.json` is the provenance, signed as the
   pipeline with `cosign attest-blob`: 149 links in all. `release-sign` finds every
   file in the provenance with cosign, and `release-verify` holds the provenance
   and the copies to the files. Read `release-verify`'s `verification.md`: the identity must
   be `https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/vX.Y.Z`,
   or, once the project has moved to the group, `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/vX.Y.Z`.
   Note the repository id it gives: until one release signed before the move
   shows `86730559`, the project does not move (hmailserver/docs/UpdateFeed.md).
5. **The installer smoke test**, on the signed installer: download
   `release-signed/` from `release-sign`'s artifacts to the VMware host and run
   `bash build/ci/installer-smoke-run.sh <the previous release's installer>
   release-signed/hMailServer-X.Y.Z-x64.exe`, then
   `bash build/ci/installer-smoke-run.sh release-signed/hMailServer-X.Y.Z-x64.exe`
   on its own, a new installation, which is where the secure defaults are
   checked (`hmailserver/docs/SecureDefaults.md`)
   (`hmailserver/docs/ContinuousIntegration.md`, "The installer smoke test").
   Both must pass before the next step.
6. **Start `release-publish`, as the owner.** The job uploads and creates the
   release with its `CI_JOB_TOKEN`, which acts with the permissions of whoever
   started it, and the package protection rules let only an Owner push to the
   package `hmailserver`: started by anyone else, its uploads are refused
   (`hmailserver/docs/ContinuousIntegration.md`, "What waits for the owner").
   It uploads the release's 149 files - the fifty-eight, their bundles, the
   `.sigstore.json` copies and the provenance - to
   the package `hmailserver/X.Y.Z` - refusing, before it sends anything, a
   compiled file whose SBOM is missing or describes other bytes, a copy that is
   not its bundle, a provenance that does not name every file, a link GitLab
   would refuse, a site Pages or a server would not read whole, and a name
   the package holds with other bytes, and not sending again a file it holds
   with the same bytes - reads every record of each name back and holds it to
   the digest that was signed, creates the release with a
   link per file named after it, and checks that the feed the API now gives
   offers the release to Windows servers, to Debian and RPM servers on
   x86-64 and AArch64, and to the AppImages. `update-feed` then
   runs `pages` on `master`. Check the site:

   ```
   python3 build/generate-update-feed.py --check --site https://updates.progressiverobot.com
   ```

   The container image is pushed by hand, from this same pipeline, once the
   maintainers have set the project's CI/CD variable `HM_PUBLISH_IMAGE` to
   `1` - not before the project has moved to the group `hmailserver`
   (`hmailserver/docs/ContainerImage.md`, "Publishing"). Start
   `container-image-deb`, which the pipeline shows as waiting (it refuses,
   exit 3, while the variable is not `1`; start it again once it is): it takes
   the release's `.deb` and its SBOM from the published release and verifies
   both against their Sigstore bundles. The rest follow it: `container-image`
   builds the image from that `.deb` and pushes `:X.Y.Z`, and for a release
   without a suffix `:X.Y` and `:latest`; `container-image-sbom` makes its
   SBOM; `container-image-sign` signs it by digest as the same pipeline and
   attests the SBOM to it; `container_scanning` scans it; and
   `container-image-start` starts it beside PostgreSQL and asks it for SMTP
   and the REST API. Read `container-image-signed/verification.txt`, the
   scan's report and the start test's log. A check that fails after the push
   leaves tags naming an image that did not pass, which the job lists: delete
   them.

7. **The package managers** (step 13) are still by hand: the manifests job was
   not ported.

**The rehearsal.** Every protected `batch*` branch's pipeline rehearses steps 3
to 4 (`.gitlab/ci/release-rehearsal.yml`): `release-rehearsal` collects every file
as `release` does, from the same jobs - the version is `Version.h`'s, and the x64
installer `windows-packages-x64`'s, built under its commit-stamped name, since
`windows-installer` runs for a `v*` tag alone - and counts what GitLab and Pages
would take; `release-rehearsal-sign` signs every file, makes each copy and attests
the provenance with Sigstore as the branch; `release-rehearsal-verify` holds every
SBOM, copy and the provenance to the files, and has the server's own verifier read
each bundle and refuse it at the identity. Servers trust `v*` tags alone, so what a
rehearsal signs installs nowhere, and it publishes nothing: no job of it uses the
job token GitLab gives every job, and `release-chain.py` refuses `--rehearse` with
`publish`. What it cannot rehearse is the tag's own checks, the security gate,
the registry and Authenticode. A rehearsal that fails is a release that would fail: mend it before
the tag.

A tag is spent once here too. Before `release-publish`, a job that failed for
a reason outside the tree - a setting, a tool on bench150, the network - is
retried at the same tag once the cause is put right. A fault in the tree needs a
new commit and so a new version. A `release-publish` that stopped part-way, with
some files uploaded, is finished by retrying `release-publish` in the same
pipeline, which sends only what is not up yet; not by starting `release` again,
which refuses once the installer or a bundle is in the registry, since signing
again would make other bytes under the same names. After `release-publish`, a
release that is wrong is corrected by the next version, not by editing this one:
servers may already hold its files.

The pipeline signs every file with Sigstore and carries no SSH signature: the
key that signs the release tags is the owner's, on his own machine, and no
GitLab job can hold it. A `.sig` of the sums file is for a release made by hand.

## A release made by hand

From 6.3.4, a release built and published by hand - 6.3.4 itself, made after
GitHub's suspension and before the release jobs above existed, or any later one
made outside the pipeline - carries `hmailserver-<v>-SHA256SUMS.txt`, the
`<sha256>  <name>` line of every file of the release but itself and its `.sig`, and
`hmailserver-<v>-SHA256SUMS.txt.sig`, an SSH signature of it made with the key
that signs the release tags: the one key `.gitlab/allowed_signers` lists, under
the principal `christopher.j.holloway@outlook.com`. It stands where the
pipeline's Sigstore bundles would. Once the sums file is written, and before
anything is uploaded, sign it with the tag's key and check the signature
against the tag's own `.gitlab/allowed_signers`:

```
ssh-keygen -Y sign -f ~/.ssh/id_ed25519 -n file hmailserver-<v>-SHA256SUMS.txt
git show v<v>:.gitlab/allowed_signers > allowed_signers
ssh-keygen -Y verify -f allowed_signers -I christopher.j.holloway@outlook.com -n file -s hmailserver-<v>-SHA256SUMS.txt.sig < hmailserver-<v>-SHA256SUMS.txt
sha256sum -c hmailserver-<v>-SHA256SUMS.txt
```

The namespace is `file`, not git's `git`, so a tag signature cannot be passed
off as a signature of the sums, or the other way round. Both files are
uploaded and linked like every other file of the release. SECURITY.md,
"Verifying a release", gives users the same two commands.

## A security release during the pause

From 6.3.6 no feature release is made, but a confirmed vulnerability still ships
in a release: 6.3.6 with fixes, as 6.3.7, 6.3.8 and so on. Which versions are
supported, until when, and what gets a release is in SECURITY.md, "While
releases pause" (the maintainers' decision of 29 September 2026). A security
release is made by the tag's pipeline, as in
[The release on GitLab](#the-release-on-gitlab), with the steps of
[Order of operations](#order-of-operations) below it. What differs is the
order in which things become public, and the steps only the owner can take.

**Before starting**, check what the release depends on is still valid: the
table in SECURITY.md, "What must stay valid for a security release". In
particular, check four things:

- the findings of `.gitlab/security-exceptions.yml` that expire before the
  release (298 of them on 26 and 27 March 2027) have been read again, or the
  release job refuses;
- the Artifact Signing identity validation has not lapsed (the Azure portal);
- linix and bench150 are up and online on GitLab
  (`hmailserver/docs/ContinuousIntegration.md`, "The runners");
- the project's path, which the federated credential's subject names:
  `Progressiverobot/hmailserver` until the move into the group,
  `hmailserver/hmailserver` after it.

1. **Fix it without pushing.** Write the fix and its regression test, which must
   fail against the build before the fix. Then run steps 2 to 9 of
   [Order of operations](#order-of-operations) by hand, on the maintainers'
   machines: the Windows suite on a bench, and the Linux build and suite on
   linix by hand (`build/ci/linux-build.sh`, `build/ci/linux-suite.sh`). Step
   8c's fuzz run may cite the previous minor's run when no fuzzed parser
   changed. Nothing is pushed yet: the repository is public, and the fix is
   visible from the moment it is.
2. **Write the notes**: the version's `CHANGELOG.md` section, headed
   `## [X.Y.Z](...) - <date>`, committed with the stamp of step 7. The section
   says what was fixed, names the confidential issue, and credits the reporter.
   It does not say how to exploit the flaw, because it is public from the push,
   about a day before the release.
3. **Land it**, with `python3 build/land-via-mr.py batchN`: the batch's branch
   pipeline runs every gate, and Zain Ul Abidin approves the merge request.
   **Owner:** if waiting for that approval would take the fix past its
   disclosure window, land it with
   `--land-without-approval "<the issue and the date it is due>"`. The change is
   then reviewed on its merge request afterwards (GOVERNANCE.md, Review).
4. **Owner: the federated credential for the tag**, made before the tag is
   pushed, as [On GitLab, from 6.3.5](#on-gitlab-from-635) step 1 says. Use the
   project's path of that day in the subject.
5. **Owner: the tag.** Sign it with the key in `.gitlab/allowed_signers`, check
   it, and push it (step 2 of [The release on GitLab](#the-release-on-gitlab)).
   No one else holds that key, and `release` refuses a tag signed by any other.
6. **Owner: start `release`** once the tag's gates have passed. The owner is the
   only person in the project with the Maintainer role, which a manual job on a
   protected tag needs (the project access token `laptop-git` is an Owner too:
   SECURITY.md, "What must stay valid for a security release").
   `release-authenticode`, `release-sbom`, `release-sign`
   and `release-verify` then follow by themselves.
7. **The installer smoke test**, by hand, on the signed installer (step 5 of
   [The release on GitLab](#the-release-on-gitlab)), with the previous release
   and on its own.
8. **Owner: start `release-publish`.** Only an Owner's job may upload to the
   package `hmailserver`. `update-feed` then brings the site up to date. Check
   it with
   `python3 build/generate-update-feed.py --check --site https://updates.progressiverobot.com`
   and `python3 build/check-atom-feed.py --site https://updates.progressiverobot.com`.
9. **Owner: remove the federated credential** (`az ad app federated-credential
   delete`, [On GitLab, from 6.3.5](#on-gitlab-from-635)).
10. **Disclose.** Make the confidential issue public. Request the CVE ID from it,
    if that has not been done, and quote it on the issue. Answer the reporter,
    and announce the release. The update feed carries the release to every
    server whose live update is on. A server from 6.2.28 to 6.3.3 still needs
    the one `UpdateFeedUrl` line (`hmailserver/docs/UpdateFeed.md`).

**If Authenticode cannot be had in time**, for example because the identity
validation has lapsed, the pipeline cannot publish. `release-sbom`, and every
job after it, waits for `release-authenticode`. The fallback is
[A release made by hand](#a-release-made-by-hand), as 6.3.4 was. Its sums are
signed with the tag's key, and it carries no Sigstore bundle. No server installs
it by live update, and `build/generate-update-feed.py` publishes it as the
stable release only once `UNSIGNED_BY_DESIGN` names it. Say so in the notes, so
that administrators upgrade by hand.

## Order of operations

1. **Freeze the code.** No fixes land after this point without restarting at
   step 2.
2. **Adversarial review of the complete diff** — independent reviewers per
   dimension (delivery paths, protocol handling, UI truthfulness vs. code,
   docs claims vs. code), each finding verified by skeptics instructed to
   refute it. This happens **before** version stamping, not before tagging:
   the 6.2.15 review ran after the installer was built and both blockers it
   found (a BDAT truncation defect and a fix that converted transient file
   locks into permanent mail loss) forced rebuilds and a re-run of the full
   suite. Same review, an hour cheaper, if run first.
3. **Fix what survives review; each defect fix gets a negative-control test.**
   Build the pre-fix binary and confirm the new test *fails* against it. A
   test that passes on both builds proves nothing — one of the 6.2.15 abort
   tests did exactly that until it was strengthened with session-count
   assertions.
4. **Pre-flight the bench**: first `wsl -l --running`, and `wsl --shutdown` if
   anything is up. With mirrored networking a Linux server or test run left
   behind inside WSL holds the suite's ports invisibly - the Windows TCP table
   shows nothing - and the pre-flight probes only the TLS fixtures' twelve, so
   it passes. An orphaned Linux bench cost the first 6.3.1 assertion gate 24
   errors this way; the Linux bench is the VM now, not WSL, for this reason.
   Then `build\preflight-tests.ps1 -Clean` and restart the service, every time:
   a *completed* suite leaves `PreferredHashAlgorithm=4` in the ini (a fixture
   restores the default by writing it rather than removing the key), so the
   next pre-flight fails until it is cleaned, and an aborted run leaves a
   deliberate scanner error in the ERROR log that fails 100% of the next
   run's tests in fixture setup, and the TLS fixtures' twelve extra ports
   behind, which the pre-flight checks. Gate on its exit code rather than
   reading its output: a chain that started the suite after a failed
   pre-flight did exactly that once.
5. **Check the roadmap against itself**: `build\check-roadmap.ps1`. It
   reconciles every tick box in `Roadmap.md` against the per-section counts
   and the contents table, because three hand-edited restatements of the same
   750 numbers drift — nine sections had drifted when the check was written.
6. **Check the database scripts build a database**: `build\check-db-scripts.ps1`,
   run with `powershell.exe` and not `pwsh` - SQL Server Compact's managed
   provider cannot load its native components under .NET on PowerShell 7, which
   fails as "Unable to load the native components ... of version 8876" and says
   nothing about the scripts. It creates a throwaway SQL CE database from `CreateTablesMSSQL.sql` using the
   same splitting rules the server uses, because nothing else here does - the
   regression bench's database is upgraded out of band, so no local test takes
   the CREATE path a fresh install takes. 6.2.22-pre4 shipped an installer whose
   database could not be created at all, and the only symptom was a service that
   started and did not listen.

   Run `build\check-schema-versions.ps1` beside it (pwsh 7, not Windows
   PowerShell 5.1, where it dies in `Measure-Object -Property To`). It walks the
   registered upgrade chain and proves it is contiguous and forward-only, and that
   every step is present for every backend. That matters most on exactly the kind
   of release this checklist keeps being used for - one that moves the schema
   several steps past the last stable - and it was missing from this step until
   6.2.23.

7. **Version stamp**, twelve files. The Windows nine: `Version.h` (version,
   numeric, build), `section_setup_64.iss`, and all seven `.csproj`
   `<Version>` values. Then three outside the Windows build:
   * `hmailserver/source/Tools/Cli/HmailServer.psd1` - `ModuleVersion`, the
     PowerShell module's version on the PowerShell Gallery. A data file cannot
     read `Version.h`, so it is written; `build/check-cli-packages.py` fails the
     `checkers` job while it differs. The PyPI package `hmailserver-cli` is not on
     this list: `build/ci/cli-packages.py` takes its version from `Version.h` when
     it builds it ([CommandLinePackages.md](hmailserver/docs/CommandLinePackages.md)).
   * `hmailserver/source/Server/platform/packaging/PKGBUILD` - `pkgver`, which
     is also the tag the PKGBUILD fetches. It cannot be derived: `makepkg`
     parses the PKGBUILD as a shell script and never runs CMake.
   * `hmailserver/source/Tools/ImportTool.Tests/packages.lock.json` - the entry
     for the project's own version. Regenerate it (`dotnet restore
     --force-evaluate` on that project) rather than editing it by hand.
     **Nothing enforces this one.** Measured on 10 September 2026: `dotnet
     restore --locked-mode` with a stale project-reference version exits 0,
     because locked mode validates the package graph and not project
     references - so a stale entry ships a lock file that lies about the tree,
     and this step is the only thing that catches it.

   `hmailserver/source/Server/CMakeLists.txt` is **not** on the list, and this
   is the one line of this step worth reading twice: it used to be, and a stamp
   missed there produced Linux packages named at the wrong version, which
   the release jobs (`build/ci/release-chain.py`; the GitHub workflow
   `sign-release.yml` up to 6.3.3) and the server's own update checker - all of
   which match those file names by exact equality - would simply never find. It now reads
   `Version.h` with `file(STRINGS)` before `project()`, so there is nothing
   there to stamp.

   Then run **`build/check-linux-version-stamp.sh`** (Git Bash, WSL or the
   runner; it needs no toolchain, which is why the `checkers` job of
   `.gitlab-ci.yml` runs it before anything is compiled, and `linux-build` again
   before it packages). It proves Version.h agrees with
   itself, that the CMakeLists still derives rather than declares, that
   `pkgver` matches, and that no version literal has crept back into anything
   else under `platform/packaging/`.

   Verify nothing else still carries the old version: `git grep <old-version>`.
   What legitimately stays behind is prose of the form "new in 6.2.28", which
   names when something arrived and must not be moved forward.
   In `section_setup_64.iss` the `VersionInfoVersion` fourth component stays
   `0`: the build number lives in `Version.h` and the tag, and a build-only
   re-cut (the common case) must not have to touch the installer script or
   the seven `.csproj` files, whose versions are also `<version>.0`.
8. **Build everything at the stamped version**: `build.ps1 -Configuration
   Release`, `build-tools.ps1 -Configuration Release`, ControlPanel
   `dotnet publish` to its `publish\` folder (build-tools does not cover it),
   `build-tests.ps1`. Confirm the stamped `FileVersion` on
   `hMailServer.exe` and `publish\hMailCP.dll`.

   The COM wrapper the tools compile against (`source\Tools\Interop\
   Interop.hMailServer.dll`) is not in git since 11 September 2026: `build.ps1`
   generates it from the type library it just built, and `build-tools.ps1`
   generates it if it is missing or stale, both through
   `build\generate-com-wrapper.ps1`, which rewrites it only when the type
   library changed. So the order above - server, then tools - is what keeps the
   shipped wrapper current after an IDL change, and there is nothing to
   regenerate by hand and no manifest hash to update. An interface-ordering fix
   still has to land BEFORE the server build that the tools are built after,
   for the same reason as ever: the wrapper freezes the vtable layout of the
   type library it was made from.

   Then prove the build is still reproducible: build Release a second time
   from clean (`build\build.ps1 -Configuration Release -Clean`) and compare
   the SHA-256 of `hMailServer.exe` with the first; then build the same
   tree once more from clean in a second checkout at another path and
   compare that too. The version stamp is not committed yet, so make the
   checkout from `git stash create`, which records the working tree as a
   commit and changes nothing in it
   (`$tree = git stash create; if (-not $tree) { $tree = 'HEAD' }`, then
   `git worktree add --detach ..\hm-repro $tree`), and run
   `build\get-installer-binaries.ps1 -SystemOnly` in it, as in any new
   clone. All three must be identical - /Brepro,
   /d1trimfile (the checkout's root and the libraries' root), /pdbaltpath
   and OPENSSL_NO_FILENAMES in the project make the executable a function of
   the source, the toolchain and the libraries, and a mismatch means
   something has started embedding a timestamp or a path again. Two builds
   in one directory cannot show a path: until 26 September 2026 they were
   identical while a checkout elsewhere built another executable
   ([ReproducibleBuilds.md](hmailserver/docs/ReproducibleBuilds.md)). Put
   the hash in the release notes' verification section so anyone
   with the same toolchain (v145, Windows SDK 10.0.26100) and library layout
   can check the published binary came from the published source. The two
   builds in the release tree rewrite the `hMailServer.exe` step 9 tests
   (`build.ps1` turns the project's build events off, so neither stops or
   starts the service), so do this before step 9, never after; the build in
   the second checkout writes nothing the release uses.

   The Linux packages keep the same promise by a different mechanism, and
   what is proven is narrower, so it is stated narrowly. `CMakeLists.txt`
   passes `-ffile-prefix-map=<source>=.` so no absolute path reaches the
   binary, and the `linux-build` job of `.gitlab-ci.yml`
   (`build/ci/linux-build.sh`, as the *Linux build* workflow did on GitHub up
   to 6.3.3) exports `SOURCE_DATE_EPOCH` from the commit's own date, which is
   what `__DATE__`, the archiver, `dpkg-deb` and `rpmbuild` stamp instead of
   the clock. Two builds of the same tag in the same image therefore produce
   the same `hmailserver` binary. What is *not* pinned is what the image
   holds: its Ubuntu packages are the archive's of the day it was built (on
   GitHub, `ubuntu-latest` moved the same way), and a compiler or a Boost that
   moved between two builds changes the binary legitimately. The job log
   records the versions it built with, and the
   release notes say "reproducible on the runner image of the day" and give
   the binary's SHA-256 from that log, rather than claiming what the MSVC line
   above can claim. Measured on 8 September 2026: commit 8926f5c3f built twice
   on the hosted runners - the two attempts of run 34282935758, forty-four
   minutes apart, on two runner allocations - and every job's binary hashed the
   same both times (x86-64 clang, x86-64 GCC, AArch64 clang). Since 26
   September 2026 the packages are reproducible too - `CMakeLists.txt` fixes
   their directories' and the `.deb`'s maintainer scripts' modes and
   `rpmbuild`'s clock and host name, and `linux-build.sh` its umask - and the
   `linux-repro` job
   checks all three for every tag before `release` can run
   (`build/ci/linux-repro.sh`): it builds the commit a second time with
   `linux-build.sh`, from a copy of the tree at another path with other file
   modes, in another build directory, started under another umask and in
   another time zone, and requires the binary, the `.deb` and
   the `.rpm` to be byte for byte `linux-build`'s
   ([ReproducibleBuilds.md](hmailserver/docs/ReproducibleBuilds.md)). The
   AArch64 `.deb` and `.rpm` a release carries from 6.3.6 are the package
   jobs', built once and not compared.

8b. **Full regression suite on the assertion build, first.** Build with
   `build\build.ps1 -Configuration Release -Asserts` - the same source, with
   every `HM_ASSERT` kept and a violated one reported as HM6364 in the ERROR
   log instead of compiled out - and run the whole suite on it. The ERROR log
   is checked before each test, so a violation fails the test that provoked
   it and names the expression, file and line. Expect none. Then rebuild plain
   Release (step 8) before step 9: the assertion build is the dynamic-analysis
   build and is never the binary that ships or is hashed.

8c. **Timed fuzz run on the release source** — required for every minor
   release. On the build from step 8: `fuzz\run-fuzz.ps1 -Target
   mime_message_fuzzer -Minutes 30`, and the same for each other harness
   listed in `hmailserver/docs/Fuzzing.md`. A crash, a hang or a violated
   assertion is a release blocker: minimise it, fix it, add the input to
   `fuzz\regression`, restart at step 1. Record the harnesses, the duration
   and the execution count in the release notes' verification line. A patch
   release may cite the previous minor's run when none of the fuzzed parsers
   changed; anything else runs again.

9. **Full regression suite on the stamped binary** (`build\run-tests.ps1`,
   with no `-StopOnError`) — every test, nothing skipped. If *anything* changes after this run, the run is void: rebuild
   and re-run. Never abort a run; if one must be stopped, expect step 4 to
   fail and clean up before trusting any result.
   At this point the reports of the commit's pipeline - *dependency-scan*,
   *static-analysis*, *semgrep* and *semgrep-sast*
   (`hmailserver/docs/ContinuousIntegration.md`, "Security scanning") - pass
   `build/ci/security-gate.py`, which holds them to the thresholds in
   `SECURITY.md` ("Before a release") and to `.gitlab/security-exceptions.yml`,
   or the release waits. The release job runs the gate itself; a release made by
   hand runs the scans on linix against the release's commit and the gate over
   what they wrote (`python3 build/ci/security-gate.py --artifacts <dir>`).
   Up to 6.3.3 these were GitHub's Code Scanning and Dependabot alert pages.
10. **README release notes** — every claim checked against the diff. "Fixed"
   means reproduced-then-fixed or negative-control-tested; anything else is
   described as hardening or diagnostics. Unfixed known issues are named as
   unfixed.
11. **Installer**: ISCC on `hMailServer64.iss`. Three prerequisites the script
   does not check: `$env:hMailServerLibs` must point at the library tree,
   `build\get-dotnet-runtime.ps1` must have populated `installation\DotNet\`
   with the desktop runtime the installer carries - the one release
   `hmailserver/docs/third-party-binaries.json` pins, checked against the
   SHA-512 Microsoft publishes for it (`-Verify` checks the file in place, and
   refuses an older download left from before the pin) - and
   `build\get-installer-binaries.ps1` must have placed the third-party binaries
   - the MSVC runtime gathered from Visual Studio, the ADO type libraries copied
   from Windows, and 7-Zip, the MariaDB client and the SQL Server Compact
   runtime fetched from the `build-inputs-1` release -
   none of which is in git since 11 September 2026 (run it with `-Verify` to
   check; every file is matched against `hmailserver/docs/third-party-binaries.json`). Never run the installer on
   the dev machine — validation is the installer smoke test, which installs it
   on a throwaway machine and must pass **before** the release is published:
   on GitLab, step 5 of [The release on GitLab](#the-release-on-gitlab)
   (`hmailserver/docs/ContinuousIntegration.md`, "The installer smoke test").
   Up to 6.3.3 it was the GitHub workflow `installer-smoke.yml` (in the git
   history at `v6.3.3`), dispatched by hand on a hosted runner once the draft
   release carried its installer:
   `gh workflow run "Installer smoke test" -f release_tag=vX.Y.Z`.
12. *History: how a release up to 6.3.3 was committed, tagged, published and
    signed on GitHub, by the workflows that are in the git history at
    `v6.3.3`. From 6.3.5, [The release on GitLab](#the-release-on-gitlab)
    replaces this step from the tag on.*

    **Commit** (as chrisholloway5, no co-author trailers - history has been
    rewritten once to remove them, and will be again). `master` is protected:
    changes arrive by pull request with an approving review from someone other
    than the last person to push, force-pushes and deletions are refused, and
    an administrator's bypass exists for emergencies, not for the release flow.
    So push the working branch, open the PR, have the other maintainer approve
    it, and merge it with a rebase so the history stays linear and the commits
    keep their own messages:

    ```
    git push origin server-fixes-wave
    gh pr create --base master --head server-fixes-wave --fill
    gh pr merge --rebase            # after the other maintainer's approval (GOVERNANCE.md, Review)
    git pull --ff-only origin master
    ```

    Then **tag** `vX.Y.Z` on master as an **annotated, signed** tag and push it
    (tags matching `v*` are protected too: no deletion, no rewrite - a tag is
    spent once):

    ```
    git tag -s vX.Y.Z -m "hMailServer X.Y.Z"
    git -c gpg.ssh.allowedSignersFile=.gitlab/allowed_signers verify-tag vX.Y.Z
    git push origin vX.Y.Z
    ```

    Signing is SSH-based (`git config gpg.format ssh` and `user.signingkey`
    pointing at the public key, set once in the repository's config), and the
    key must be listed in `.gitlab/allowed_signers` (`.github/allowed_signers`
    until the move to GitLab) - the signing workflow
    below refuses to sign anything for a tag that is lightweight or that does
    not verify against that file, so an unsigned tag stops the release before
    it has any assets. Every tag up to `v6.2.23-alpha1` was either lightweight or annotated but
    unsigned; there was nothing to verify and nothing stopped a `v*` ref being moved.

    A pre-release's tag carries a suffix - `v6.3.5-rc1`, never `v6.3.5` with
    "Pre-release" in its title. The update feed and every server built after
    6.3.3 tell a pre-release by that suffix alone, since neither the feed's flag
    nor a title is the signer's word, and the feed does not publish a release
    titled as a pre-release whose tag has none (docs/UpdateFeed.md).

    Then publish **as a draft first** - this repository has
    immutable releases enabled, so a published release refuses every further asset
    upload and would be stuck with whatever it was created with:

    ```
    # --prerelease ONLY for an alpha, beta or release candidate. A stable
    # release must not carry it: the shipped update checker reads GitHub's
    # "latest release" endpoint, which never answers with a pre-release, so a
    # stable release left marked pre-release is invisible to every server in
    # the field. If one is created that way, `gh release edit vX.Y.Z
    # --prerelease=false` before publishing.
    gh release create vX.Y.Z <installer> --draft \
       --title "..." --notes-file <notes>

    # The tag's own Linux run builds the six Linux artefacts - a .deb, an .rpm
    # and an .AppImage per architecture - and its publish job attaches them and
    # a SHA-256 sums file to this release itself. Watch it rather than
    # downloading artefacts:
    gh run list --workflow "Linux build" --branch vX.Y.Z
    gh run watch <run-id>
    # Authenticode, if a certificate has been arranged - see below. The job is
    # part of "Sign release artefacts" and does nothing while the settings are
    # absent, writing a notice instead.
    gh workflow run "SBOM" -f release_tag=vX.Y.Z          # SPDX + CycloneDX
    gh workflow run "Sign release artefacts" -f tag=vX.Y.Z  # LAST: signs what is attached
    gh workflow run "Installer smoke test" -f release_tag=vX.Y.Z   # green before publishing
    # Expect: the installer, 2 .deb, 2 .rpm, 2 .AppImage, one SHA256SUMS.txt,
    # 2 SBOMs, a .cosign.bundle AND a .sigstore.json (the same bytes, under the
    # name OpenSSF Scorecard recognises) beside every one of them, and one
    # hmailserver-vX.Y.Z.intoto.jsonl of SLSA provenance - THIRTY-ONE assets.
    # Count them. This step exists because an artefact that failed to build
    # goes missing quietly, and the release is immutable once published.
    gh release view vX.Y.Z --json assets
    gh release edit vX.Y.Z --draft=false                   # publish, now complete
    ```

    **Linux packages: one release, every platform.** Pushing the tag started
    the *Linux build* workflow, whose package jobs keep one `.deb` and one
    `.rpm` per architecture as run artefacts for fourteen days. They join the
    draft beside the installer, BEFORE the SBOM and signing steps, because
    both act on whatever is attached when they run. Their names are as fixed
    as the installer's: `hmailserver_<version>_amd64.deb`,
    `hmailserver_<version>_arm64.deb`, `hmailserver-<version>-1.x86_64.rpm`
    and `hmailserver-<version>-1.aarch64.rpm`. A Linux server's update checker
    builds the one its own package manager and architecture would install and
    matches it exactly, as a Windows server does the installer, so CPack's
    names are uploaded as they are and never tidied. The signing workflow
    lists which platforms a release carries and *warns* about a missing Linux
    set rather than failing, because the Windows installer is what every
    server in the field is waiting for and a Linux packaging failure must not
    hold it back; a release that ships without them is a Windows-only release
    and its notes say so. The AppImages are attached too, and are the one
    asset here that is not for installing: an AppImage of a daemon is a way to
    try the server without putting anything on the machine, and the build
    script says so on its way out. The PKGBUILD is not a release asset at all -
    it is built by the user's own machine from the tag.

    **The installer's name is not cosmetic.** It must be exactly
    `hMailServer-<tag without the leading v>-x64.exe`, with its `.cosign.bundle`
    beside it. The update checker in every running server builds that name from
    the tag and matches an asset on exact equality; a release named anything else
    reports "a new version is available" and then "there is nothing to download"
    to every installation in the field. This was broken once already, at
    `v6.2.22-pre3`, whose installer was `hMailServer-6.2.22-x64.exe`. The signing
    workflow now refuses a release that does not carry the expected name, so the
    check happens before publication rather than after somebody notices.

    Order matters twice over: the SBOMs have to be on before signing, because
    cosign signs whatever is attached when it runs; and everything has to be on
    before publication, because nothing can be added afterwards. 6.2.22-pre4 was
    published straight away and ended up with an installer and no SBOM at all.

    **A published release is immutable.** This repository has immutable
    releases on: once published, a release's assets and tag cannot be added to,
    changed or removed (the API answers 422 to an upload). So a signature, a
    `.sigstore.json` twin or the provenance can only be attached before
    publication, which is when the signing workflow attaches them; a release
    that shipped before the twins and the provenance existed (6.3.1 and
    earlier, and `build-inputs-1`) stays as it is, and OpenSSF Scorecard's
    Signed-Releases check, which judges the five latest releases, climbs as
    new releases replace them.

    **The two `gh workflow run` lines are required, not belt-and-braces.** Saving a
    draft does not start a workflow: GitHub does not deliver `release` events for
    draft releases, which was measured here rather than assumed - creating the pre5
    draft produced no run at all. The `release: created` trigger on both workflows
    therefore only covers a release published without a draft, which is the path
    that can no longer attach anything. Dispatching them by hand is the path that
    works.

    And a tag is spent once: a tag that has backed an immutable release cannot back
    another, even after that release is deleted. There is no re-cutting a broken
    release under the same version - it needs the next number.
13. *History: the GitHub workflow `package-managers.yml`, in the git history
    at `faca023d4`; it was never ported to GitLab
    (`hmailserver/docs/ContinuousIntegration.md`, "From GitHub Actions to
    GitLab CI"), so the manifests are rendered and submitted by hand.*

    **The package managers**, once the release is published and its installer
    signed. The *Package managers* workflow runs itself on `release: published`:
    it downloads the installer the release actually attached, refuses it unless
    its Authenticode signature is valid, computes the SHA-256 from those bytes,
    renders the three winget manifests and the Chocolatey package from the
    templates in `hmailserver\installation`, validates them against Microsoft's
    published schemas and packs the `.nupkg`. The files are kept as an artifact
    of the run.

    Submitting them is deliberately not automatic until it has been armed once:
    a new package identifier is reviewed by a person at microsoft/winget-pkgs,
    so the FIRST submission is by hand. `hmailserver\docs\PackageManagers.md` is
    the walk - the fork, the token, `wingetcreate submit`, the Chocolatey API
    key - and says what to set (the `PACKAGE_MANAGER_SUBMIT` variable, the
    `WINGET_TOKEN` and `CHOCO_API_KEY` secrets) so that every release after
    that submits itself.

    The hash is computed there and never copied from a release note, because the
    signed installer is not the built installer: signing changes the bytes, and a
    package manager handing out a mismatched hash is a broken install for every
    user of it.
14. **Close the loop**: answer every issue the release resolves (and close
    them), update the ones it does not resolve saying so plainly.

## Standing rules

- **Prefer two small releases over one large one.** 6.2.15 carried IMAP
  semantics, delivery changes, a connection-layer rewrite and a UI expansion
  in one tag; its worst defect lived in the *interaction* between two of
  those themes.
- **A fix to a shared layer needs a dependents audit.** Before changing
  behaviour in something like `TCPConnection`, enumerate who depends on the
  old behaviour (the BDAT reads depended on "any error ends the session").
- **The public record never overstates.** If it is not reproduced and
  negative-control-tested, it is not "fixed" — say "hardened", "instrumented"
  or "still open". Credibility with reporters is the project's scarcest
  resource.

Authenticode
------------

Every release asset is signed with cosign, keylessly, and that signature is what
somebody who deliberately checks can verify. It is not what Windows reads.
SmartScreen and the UAC prompt read **Authenticode**, which the Windows
installer has carried since 6.3.1 (before that they showed an unknown
publisher) - so the two are complementary.

**Be clear what it buys, because it is not what everybody assumes.** Signing does
not remove the SmartScreen warning. Microsoft's own comparison puts a signed
installer in the same row as an unsigned one - flagged as unrecognised until
reputation accumulates - and reputation attaches to a file that does not change,
which a new 80 MB installer every few weeks never is. What signing buys is two
things that are real: the elevation prompt reads the publisher's name instead of
*Unknown publisher*, from the first signed release; and an enterprise policy that
refuses unsigned binaries outright stops being a wall. For a mail server, whose
users are disproportionately administrators on policy-managed Windows, the second
is the one that matters.

### On GitLab, from 6.3.5

The `release-authenticode` job (`build/ci/windows-authenticode.ps1`, on bench150)
signs the installer with **Azure Artifact Signing** through SignTool and
Microsoft's Artifact Signing plugin, with an RFC 3161 timestamp from
`http://timestamp.acs.microsoft.com`, and holds no secret: the job's GitLab ID
token, with the audience `api://AzureADTokenExchange`, is what Microsoft Entra
ID accepts through a federated credential in place of a client secret. It stops
before sending anything while a setting is missing, naming it, and when Entra
refuses the token it prints Entra's own answer, which names the subject the
token carried. What the owner creates:

1. **A federated credential for each release, made for its tag alone**, on the
   app registration that holds the *Artifact Signing Certificate Profile
   Signer* role on the certificate profile (the one used for 6.3.1 to 6.3.4
   will do), before the tag is pushed: issuer `https://gitlab.com`, subject
   `project_path:Progressiverobot/hmailserver:ref_type:tag:ref:vX.Y.Z` - a
   GitLab ID token's subject for that tag, which Entra ID matches exactly -
   and audience `api://AzureADTokenExchange`. The portal has it under the app
   registration's Certificates & secrets, Federated credentials, *Other
   issuer*; or save this as `credential.json`:

   ```json
   {
     "name": "gitlab-hmailserver-vX.Y.Z",
     "issuer": "https://gitlab.com",
     "subject": "project_path:Progressiverobot/hmailserver:ref_type:tag:ref:vX.Y.Z",
     "audiences": ["api://AzureADTokenExchange"]
   }
   ```

   ```
   az ad app federated-credential create --id <the app registration's application id> --parameters credential.json
   ```

   Remove it once the release is published (`az ad app federated-credential
   delete --id <application id> --federated-credential-id
   gitlab-hmailserver-vX.Y.Z`): an app registration holds at most twenty, and
   one left in place would let another pipeline of the same tag sign. A
   pre-release's tag, `vX.Y.Z-rc1`, needs its own.

   Not a *flexible* federated credential matching `ref:v*` (in preview on 26
   September 2026), though it would save the step: `v*` matches every tag
   the protected tag rule lets a Maintainer push, and release tags are the
   owner's alone ([The release on GitLab](#the-release-on-gitlab)). A
   credential made for one tag, when the owner makes that tag, keeps the
   Authenticode signature to the owner's tags whatever the tagged commit
   carries, since `release`'s own checks run as the tagged commit has them.
   It does not hold the Sigstore signature, which any pipeline of a protected
   `v*` tag is given and which is all a server requires unless
   `UpdateRequireAuthenticode=1` is set.

   Setting the project's ID token subject to begin with its id instead of its
   path (the project's `ci_id_token_sub_claim_components`, `["project_id",
   "ref_type", "ref"]`, through GitLab's API) makes the subject
   `project_id:86730559:ref_type:tag:ref:vX.Y.Z`, which no rename or transfer
   can give to another project, and the credential's subject is then that.
   Sigstore does not use the subject, so this changes nothing for the bundles.
2. **Five CI/CD variables** of the project (Settings, CI/CD, Variables), each
   *protected*, so that only a protected ref's pipeline - a `v*` tag - reads
   them. None is a secret: `AZURE_TENANT_ID`, `AZURE_CLIENT_ID` (the app
   registration's application id), `ARTIFACT_SIGNING_ENDPOINT`
   (`https://neu.codesigning.azure.net`), `ARTIFACT_SIGNING_ACCOUNT` and
   `ARTIFACT_SIGNING_PROFILE`. The pipeline needs no client secret; the
   `AZURE_CLIENT_SECRET` of the GitHub workflow has no counterpart here.
3. **The Artifact Signing Client Tools on bench150**, for the machine, as an
   administrator. Installed on 26 September 2026, in
   `C:\Program Files\Microsoft\ArtifactSigningClientTools`; winget's
   `--scope machine` finds no installer for them, so the MSI is installed with
   `msiexec` and `ALLUSERS=1` (`hmailserver/docs/ContinuousIntegration.md`,
   "bench150", has the command).

The job then checks the signature is `Valid`, carries the timestamp, and left
the version resource as it was, before `release-sign` signs the same bytes with
Sigstore. What signing buys and what it does not is the same as below.

### On GitHub, up to 6.3.3

The `authenticode` job in `sign-release.yml` (the GitHub workflow, in the git
history at `v6.3.3`) signed the installer with **Azure Artifact Signing** - the
service formerly called Trusted Signing. The rest of this section is written as
it stood then. The job does
nothing at all until these exist. It is deliberately silent-but-visible: it
writes a notice on every release saying the installer is unsigned, and lets the
release proceed. It runs BEFORE the cosign job, which is not arbitrary: it
replaces the installer asset, and cosign has to sign the bytes that ship.

* A **pay-as-you-go** Azure subscription - free, trial and sponsored
  subscriptions are refused - with an **Artifact Signing** account and a
  certificate profile. Microsoft does the identity validation and holds the key:
  there is no certificate file to store, steal or expire in a drawer, and none of
  the 460-day reissue treadmill an owned certificate now carries. The account,
  its resource group and its subscription cannot be changed afterwards, so choose
  them as permanent.
* Repository secrets `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`,
  `AZURE_CLIENT_SECRET`, for an app registration holding the *Artifact Signing
  Certificate Profile Signer* role on that profile. The roles were renamed with
  the service and the old names do not resolve. Two more things the portal
  does not say: creating the identity validation needs the *Artifact Signing
  Identity Verifier* role on the account - Owner is not enough, because it is
  a data action, and without it the portal simply offers nothing to create -
  and the validation itself has no ARM resource type, so it is portal-only,
  while the certificate profile *is* scriptable (`certificateProfiles`,
  api-version 2025-10-13).
* Repository variables `ARTIFACT_SIGNING_ENDPOINT`, `ARTIFACT_SIGNING_ACCOUNT`,
  `ARTIFACT_SIGNING_PROFILE`. The endpoint is a full URI, not a bare host:
  `https://neu.codesigning.azure.net`, which is the action's own documented
  form. There is no UK region, and North Europe is the nearest of the sixteen
  the resource provider offers.

All six are required, and the job refuses a subset rather than signing with one:
setting four of them and leaving two blank was, until the gate was rewritten, a
release that failed at the Azure action and took the cosign job down with it.

Identity validation takes **one to twenty business days** and Microsoft states it
cannot be expedited, so this is arranged once and long before a release rather
than during one. It also has to be renewed, and signing stops if it lapses. It is
an organisation validation against Companies House plus a one-off photo-ID check
of the person applying; the verification email expires in seven days and cannot
be re-sent.

The certificates this service issues live about seventy-two hours, which is why
`timestamp-rfc3161` in the job is not optional however the Action's input is
marked: without a countersigned timestamp the signature stops verifying three
days later.

