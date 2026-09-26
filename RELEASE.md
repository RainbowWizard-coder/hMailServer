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
two, and carries no Sigstore bundle. The bench steps run as written; step 9
reads the scan reports of the commit's GitLab pipeline where it read GitHub's
Code Scanning and Dependabot pages up to 6.3.3. Step 12's commit rule is out of
date as well: since 24 September 2026 commits are made as Christopher
Holloway, the identity of the GitLab account `Progressiverobot`, and since 14
September 2026 no approval is required, so a tree that has passed the gate lands
on `master` by fast-forward, with no pull request.

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

1. **Steps 1 to 10 below, as written**, with one change: the release notes are
   the version's section of `CHANGELOG.md`, headed `## [X.Y.Z](...) - <date>`,
   committed before the tag, because `release-publish` makes the release's notes
   from it as the tag's own tree has it and refuses a version without one - a
   pre-release's too, `## [X.Y.Z-rc1](...)`. The installer is not built by hand
   (step 11): the pipeline builds it, in bench150's checkout, and refuses to if
   that checkout has a change of its own to a tracked file, or a file git does
   not track where a build would take it in (`build/ci/windows-installer.ps1`
   says which).
2. **Tag the release on `master`**, annotated and signed, and push the tag:

   ```
   git tag -s vX.Y.Z -m "hMailServer X.Y.Z"
   git -c gpg.ssh.allowedSignersFile=.gitlab/allowed_signers verify-tag vX.Y.Z
   git push gitlab vX.Y.Z
   ```

   A pre-release's tag carries a suffix, `vX.Y.Z-rc1` (step 12 says why).
   The tag's pipeline runs every gate - the checks, both suites, the package
   jobs, the SBOMs - and `windows-installer`, which builds the installer on
   bench150 after `windows-gate` and signs nothing.
3. **Start `release`** once every job before it has passed, within three days of
   the tag's pipeline (`linux-build`'s artifact is kept three days; retry
   `linux-build` otherwise). GitLab offers the button only when every job that
   can fail the pipeline has passed; it does not wait for the scans that are
   `allow_failure` (`static-analysis`, `semgrep`, `ci-image-packages`,
   `dependency-scan`, `repo-hygiene`) or for the manual `db-matrix`, so read the
   scans first. It refuses, naming why, a tag that is lightweight, not signed by
   a key in `.gitlab/allowed_signers`, or moved to another commit since the
   pipeline started, a `Version.h` that does not say the tag's version, a cosign
   image that is not `gcr.io`'s pinned by digest, a missing Azure setting, and a
   missing file. Nothing has been signed or sent when it refuses.
4. **`release-authenticode`, `release-sign` and `release-verify` follow by
   themselves.** Read `release-verify`'s `verification.md`: the identity must
   be `https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/vX.Y.Z`.
5. **The installer smoke test**, on the signed installer: download
   `release-signed/` from `release-sign`'s artifacts to the VMware host and run
   `bash build/ci/installer-smoke-run.sh <the previous release's installer>
   release-signed/hMailServer-X.Y.Z-x64.exe`
   (`hmailserver/docs/ContinuousIntegration.md`, "The installer smoke test").
   It must pass before the next step.
6. **Start `release-publish`.** It uploads the six files and their bundles to
   the package `hmailserver/X.Y.Z`, reads them back, creates the release with a
   link per file named after it, and checks that the feed the API now gives
   offers the release to Windows, Debian and RPM servers. `update-feed` then
   runs `pages` on `master`. Check the site:

   ```
   python3 build/generate-update-feed.py --check --site https://updates.progressiverobot.com
   ```

7. **The package managers** (step 13) are still by hand: the manifests job was
   not ported.

A tag is spent once here too. Before `release-publish`, a job that failed for
a reason outside the tree - a setting, a tool on bench150, the network - is
retried at the same tag once the cause is put right. A fault in the tree needs a
new commit and so a new version. After `release-publish`, a release that is
wrong is corrected by the next version, not by editing this one: servers may
already hold its files.

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

7. **Version stamp**, eleven files. The Windows nine: `Version.h` (version,
   numeric, build), `section_setup_64.iss`, and all seven `.csproj`
   `<Version>` values. Then the two nothing on Windows looks at:
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
   the SHA-256 of `hMailServer.exe` with the first. They must be identical -
   /Brepro, /d1trimfile, /pdbaltpath and OPENSSL_NO_FILENAMES in the project
   make the executable a pure function of the source and the toolchain, and
   a mismatch means something has started embedding a timestamp or a path
   again. Put the hash in the release notes' verification section so anyone
   with the same toolchain (v145, Windows SDK 10.0.26100) and library layout
   can check the published binary came from the published source. Both
   builds restart the service, so do this before step 9, never after.

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
   same both times (x86-64 clang, x86-64 GCC, AArch64 clang). The package containers are
   not compared; the binaries inside them are what the hashes above cover.

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
   *static-analysis* and *semgrep* (`hmailserver/docs/ContinuousIntegration.md`,
   "Security scanning") - show no open high or critical finding - the
   thresholds in `SECURITY.md` - or the release waits. Up to 6.3.3 these were
   GitHub's Code Scanning and Dependabot alert pages.
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
    at `6399e08a5`; it was never ported to GitLab
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
token carried. What the owner creates, once:

1. **The federated credential**, on the app registration that holds the
   *Artifact Signing Certificate Profile Signer* role on the certificate profile
   (the one used for 6.3.1 to 6.3.4 will do). A GitLab ID token's subject for a
   tag is `project_path:Progressiverobot/hmailserver:ref_type:tag:ref:vX.Y.Z`,
   and its issuer `https://gitlab.com`. An ordinary federated credential matches
   one subject exactly - Entra ID allows no wildcard in it - so either:

   * **a flexible federated credential** (in preview on 26 September 2026),
     which matches every release tag at once and, as Entra requires for GitLab,
     the project's id as well. Save this as `credential.json` and post it to the
     app registration with the Azure CLI, as someone who may edit it:

     ```json
     {
       "name": "gitlab-hmailserver-release-tags",
       "issuer": "https://gitlab.com",
       "audiences": ["api://AzureADTokenExchange"],
       "claimsMatchingExpression": {
         "value": "claims['sub'] matches 'project_path:Progressiverobot/hmailserver:ref_type:tag:ref:v*' and claims['project_id'] eq '86730559'",
         "languageVersion": 1
       }
     }
     ```

     ```
     az rest --method POST --uri "https://graph.microsoft.com/beta/applications/<the app registration's object id>/federatedIdentityCredentials" --headers "Content-Type=application/json" --body "@credential.json"
     ```

     The portal offers the same under the app registration's Certificates &
     secrets, Federated credentials, *Other issuer*. `v*` matches any tag the
     project protects as a release tag, and only a Maintainer can push one;
   * or, without the preview, **one ordinary credential per release**, made
     before its tag is pushed: issuer `https://gitlab.com`, subject
     `project_path:Progressiverobot/hmailserver:ref_type:tag:ref:vX.Y.Z`,
     audience `api://AzureADTokenExchange`. An app registration holds at most
     twenty, so remove the old ones.

   Setting the project's ID token subject to begin with its id instead of its
   path (the project's `ci_id_token_sub_claim_components`, `["project_id",
   "ref_type", "ref"]`, through GitLab's API) makes the subject
   `project_id:86730559:ref_type:tag:ref:vX.Y.Z`, which no rename or transfer
   can give to another project; the credential's expression then matches
   `project_id:86730559:ref_type:tag:ref:v*`. Sigstore does not use the subject,
   so this changes nothing for the bundles.
2. **Five CI/CD variables** of the project (Settings, CI/CD, Variables), each
   *protected*, so that only a protected ref's pipeline - a `v*` tag - reads
   them. None is a secret: `AZURE_TENANT_ID`, `AZURE_CLIENT_ID` (the app
   registration's application id), `ARTIFACT_SIGNING_ENDPOINT`
   (`https://neu.codesigning.azure.net`), `ARTIFACT_SIGNING_ACCOUNT` and
   `ARTIFACT_SIGNING_PROFILE`. The pipeline needs no client secret; the
   `AZURE_CLIENT_SECRET` of the GitHub workflow has no counterpart here.
3. **The Artifact Signing Client Tools on bench150**, for the machine, as an
   administrator: `winget install --exact --id
   Microsoft.Azure.ArtifactSigningClientTools --scope machine`.

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

