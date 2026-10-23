# Security Policy

## Supported versions

| Version | Supported          |
| ------- | ------------------ |
| 6.3.x   | :white_check_mark: security fixes until 30 September 2027 ([While releases pause](#while-releases-pause)) |
| < 6.3   | :x:                |

The 6.3 line is the one that receives security fixes. Its latest release is 6.3.7, published on 17 October
2026. A fix for a confirmed vulnerability ships in the next release; it is not back-ported to 6.2 or anything
older, because there are no maintenance branches. That is less of a burden than it sounds: an upgrade in place
keeps the configuration and the mail, and the database upgrade chain runs from every earlier version of this fork,
so the answer to a vulnerability in an older version is to upgrade.

From 6.3.6, releases pause: no release is planned by date until the maintainers say otherwise here. A release is
still made when a confirmed vulnerability needs one, as [While releases pause](#while-releases-pause) says below,
and it is cut from `master` with whatever finished work `master` holds then.

## Reporting a vulnerability

Report it privately, by one of these routes:

- **A confidential issue on GitLab:**
  <https://gitlab.com/hmailserver/hmailserver/-/issues/new?issue[confidential]=true>. The link ticks
  *This issue is confidential* for you; check that it is still ticked before you submit, and choose the
  **Security** template. A confidential issue is visible only to you and the project's members with the Planner
  role or above, which today means the maintainer who handles reports
  ([How a report is handled](#how-a-report-is-handled)).
- **Email** to the project's Service Desk address:
  [contact-project+hmailserver-hmailserver-86730559-issue-@incoming.gitlab.com](mailto:contact-project+hmailserver-hmailserver-86730559-issue-@incoming.gitlab.com).
  GitLab files the message as a confidential issue, and the maintainers' replies reach you by email, so no
  GitLab account is needed.
- **GitHub's private vulnerability reporting**, on the project's repository there (since 18 October 2026):
  <https://github.com/RainbowWizard-coder/hMailServer/security/advisories/new>, the repository's *Security* tab,
  *Report a vulnerability*. A GitHub account is needed. The maintainers read the reports made there.

Do **not** open a public issue, merge request or pull request on either forge, start a GitHub discussion, or edit
the wiki, for a security problem, and do not post it to the hMailServer forum. A public report starts the clock for
everyone running the server, not just for us. If a report was made public by mistake, say so by any route above and
the maintainer makes it confidential.

This file sent reports to GitHub Security Advisories on the project's old repository, under the account
`Progressiverobot`, until GitHub suspended that account on 18 September 2026. The account stays suspended: every
link to github.com/Progressiverobot, the old repository's advisories included, still answers 404; so if you reported
something there and had no answer, send it again by one of the routes above. The project's repository on GitHub
since 18 October 2026, under `RainbowWizard-coder`, is a new one: GitLab's mirror, kept by GitLab's push mirror
([Mirroring.md](hmailserver/docs/Mirroring.md)), with no Issues. The third route above is on it.

## What to include

- The version (the Control Panel shows it; on Linux, `hmailserver --version`), the platform (Windows, a Linux
  distribution, or a container image) and the database backend.
- What is affected - SMTP, IMAP, POP3, the REST API, the webmail, the Control Deck, the COM API, the installer or
  the update path - and whether an attacker needs an account, and which kind.
- How to reproduce it: the steps, a proof of concept if you have one, and the log lines around it.
- What an attacker gains.
- Whether and when you intend to disclose, and how you would like to be credited.

Redact real passwords, keys and addresses from anything you send.

## What to expect

| Stage | Target |
| ----- | ------ |
| Acknowledgement that the report was received | 5 working days |
| Initial assessment: reproduced or more information requested | 10 working days |
| Fix released for a confirmed vulnerability | 90 days from the acknowledgement |

This is a small project, not a staffed security team: it has two maintainers, and one of them currently receives
private reports (see [GOVERNANCE.md](GOVERNANCE.md#maintainers)). Those are the targets we hold ourselves to,
not a contractual commitment. If a deadline is going to slip, you will be told before it slips rather than after.

A confirmed vulnerability is fixed the way any defect here is: with a regression test that fails against the
build before the fix, and the full regression suite on the binary that ships. When the fix ships, the release
notes describe the vulnerability and credit you, and the issue is made public.

One known risk to the third target: the chain that signs and publishes a release ran on GitHub Actions until
GitHub suspended the project's account. Its replacement, the release jobs of `.gitlab-ci.yml`
([hmailserver/docs/ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md#the-release), "The
release"), first ran for 6.3.5, on 29 September 2026. Where it cannot run - the signing identity's validation
lapsed, a runner lost - a release is built and signed by hand, as 6.3.4 was ([RELEASE.md](RELEASE.md), "A release
made by hand"). If that holds a fix past 90 days, that is a slip like any other, and you will be told before the
target passes.

## Coordinated disclosure

We follow coordinated disclosure on a **90 day** timetable. The vulnerability is published when a fix ships, or
at 90 days, whichever comes first. If you intend to disclose sooner than that, say so in the report so we can
plan around it rather than discover it.

You keep credit for the finding unless you ask us not to name you. We will not ask you to sign anything, and we
do not pay bounties.

**CVE IDs.** GitLab is a CVE Numbering Authority for public projects hosted on GitLab.com. A CVE ID is requested
from the vulnerability's confidential issue, through GitLab's *Create CVE ID Request*, and quoted in the release
notes.

## While releases pause

6.3.6 was the last release planned by date, the owner's decision of 28 September 2026; the releases since are
security releases, each cut from `master`. What follows is **the maintainers' decision of 29 September 2026**,
taken under the owner's delegation. The maintainers write here when it changes.

### What is supported, and for how long

- **The 6.3 line, as its newest release:** 6.3.6, and each security release made after it (6.3.7, 6.3.8 and so
  on). A security release is the newest release with its fixes and whatever finished work `master` has taken
  since: `master` goes on taking finished work during the pause (6.3.7 carried the work under way on 13 October
  2026 and row A1 of Programme A, by the owner's decisions of 13 and 15 October 2026), and a security release is
  cut from `master` like every other release. There is still no maintenance branch, and nothing is
  back-ported. An installation of 6.3.6, 6.3.5, 6.2 or anything older is upgraded in place to the newest release, as
  [Supported versions](#supported-versions) says.
- **Until 30 September 2027.** Before that date, the maintainers write here whether support continues. If nothing
  has been written, the 6.3 line is not supported from 1 October 2027, and the table above is changed to say so.
- **Why that date.** It was as far as the project's own record reached when it was set, and the tokens still hold
  it there:
  - the GitLab tokens used by the maintainers' machine, Renovate and SonarQube Cloud expire on 26 and
    27 September 2027 ([What must stay valid](#what-must-stay-valid-for-a-security-release), below);
  - of the findings that `.gitlab/security-exceptions.yml` accepts, those accepted by 29 September 2026 expire by
    27 September 2027, the last 159 of them on 25 and 27 September 2027, and the 42 accepted after that day expire
    from 1 to 15 October 2027 (counted on 18 October 2026). After its expiry, a finding holds every release again
    until it has been read again.

  A later date would be a promise that rests on renewals nobody has made yet. A year is also long enough for
  several reports to run through the 90-day timetable one after another. One thing a release needs does not reach
  the date: the Windows licence of bench150, the bench that builds and signs each release's installer, is an
  evaluation, which ends in March 2027 and, rearmed, before 30 September 2027. So the date rests on bench150 being
  licensed or rebuilt before then, as well; S1-WINBENCH, the other Windows bench, reaches it with its one rearm,
  taken from 4 April 2027 ([What must stay valid](#what-must-stay-valid-for-a-security-release), below).
- **One date comes first.** bench150's Windows licence, an evaluation, ends in March 2027, on 15 March at the
  earliest ([What must stay valid](#what-must-stay-valid-for-a-security-release), below). From that day Windows shuts
  the bench down every hour, and without it no release is made and no batch lands but one of documents alone; so
  before then the maintainers rearm it, if a rearm is left, license it or rebuild it. Then 290 of the accepted
  findings (263 of *static-analysis*, 23 of *semgrep-sast* and 4 of *semgrep*) expire on 26 and 27 March 2027
  (counted on 18 October 2026). From the day after, the release job refuses a release while any of them is still
  found and has not been read again (`build/ci/security-gate.py`). So the maintainers read them again before
  26 March 2027, and give each a new expiry. Otherwise, a report that arrives after either date waits on the bench
  or on that reading as well as on its fix.

### What gets a release

- **Every confirmed vulnerability**, on the targets in [What to expect](#what-to-expect). The pause changes when
  features ship, not when fixes do.
  - A critical or high one ships as soon as its fix has passed the gates.
  - Medium and low ones may be held for each other, but none is held past 90 days from its acknowledgement.
- **A dependency advisory.** A high or critical advisory against a dependency the release ships is treated as a
  vulnerability in hMailServer and ships the same way, when hMailServer's code reaches the affected code
  ([Dependencies](#dependencies-software-composition-analysis)). When it does not, the advisory goes in the VEX
  document, not in a release.
- **Other defects** are fixed on `master`. They ship with the next security release, or when releases resume.

**6.3.7 was the exception.** Five of the vulnerabilities it fixes, each a confidential work item from the
project's own review, were public before it was released: their fixes, and the fixes' descriptions in
`CHANGELOG.md`, were on GitLab from the first push of the batch that carried them - 6 October 2026 for the first,
7 October 2026 for the other four - and on `master` from the batch's landing, on 7 and 9 October 2026, until 6.3.7
was released. The owner decided to push and land them before the release (on 6 October 2026 for the first, on
7 October 2026 for the others), and on 12 October 2026, knowing that they were public, to hold the release for more
of the work then planned; on 13 October 2026 he decided that it would follow once the work already under way had
been finished and folded into one batch, and on 15 October 2026 that the work already done for a later release, and
more fixes, would go into it as well; and so 6.3.7 was released.
So all five were public for longer than the day that step 3
below allows, and their descriptions say more about each flaw than that step allows. 6.3.7 also carries the work that
was under way on 13 October 2026 and, by the decision of 15 October 2026, the work already done for a later release.
The batch it was made from was pushed and tagged on the same day, by the owner's decision of 16 October 2026. The
list above, and step 3's window of about a day, stand for every release after 6.3.7.

### How a report is handled

The routes, the targets and the 90-day disclosure timetable above are unchanged by the pause. Security response
rests on Christopher Holloway ([GOVERNANCE.md](GOVERNANCE.md#maintainers)), and he is the only person who sees a
report on GitLab: GitLab shows a confidential issue to its author, its assignees and the project's members with the
Planner role or above, and since 13 October 2026 he is the only person who is a member of the project. Zain Ul
Abidin, a Developer from 26 September 2026, could see every report until that day, and is not a member since; whether
he takes the duty on, should he be a member again, is the maintainers' decision. A report made through GitHub's
private vulnerability reporting is seen by the maintainers and by the GitHub repository's administrators; the owner
carries it into a confidential GitLab issue, from which the steps below run, and the GitHub advisory stays
unpublished and, at disclosure, points to that issue.

1. **Acknowledge** within 5 working days, and **assess** within 10: reproduce it, or ask for what is missing.
2. **Fix** with a regression test that fails against the build before the fix. The fix is built and tested by
   hand on the maintainers' own machines before anything is pushed: the Windows suite on a bench and the Linux
   suite on linix ([RELEASE.md](RELEASE.md), "A security release during the pause").
3. **Push only when that has passed.** The project's pipelines run only for what is on GitLab, and its
   repository is public. So from the push of the fix until the release is published, the fix can be seen and the
   advisory cannot. That is about a day: the batch's pipeline, then the tag's. Keep that window short: the fix is
   pushed with the batch the release is made from, not before ([What gets a release](#what-gets-a-release) says
   where 6.3.7 did otherwise). The commit message and the fix's entry in `CHANGELOG.md`, in the version's section
   or in Unreleased, which are public from the push, say what was fixed and credit the reporter. They do not say
   how to exploit it.
4. **Release** it ([How a security release is made](#how-a-security-release-is-made), below).
5. **Disclose** once the release is on the update feed: make the issue public, request the CVE ID from it if that
   has not been done, and announce the release.

### How a security release is made

A security release is a release like any other, made by the tag's pipeline ([RELEASE.md](RELEASE.md), "A security
release during the pause", has the steps). Five of the steps are the owner's, and nobody else can take them: no
other person, and, since the project access token `laptop-git`, which held the Owner role, was revoked, no token
but the owner's own ([What must stay valid](#what-must-stay-valid-for-a-security-release), below).

1. **Landing the fix.** The owner lands the batch's merge request (`build/land-via-mr.py`) once a second person
   has approved it. When waiting would take a fix past its disclosure window, he may land it without that
   approval, giving the reason (`--land-without-approval`, [GOVERNANCE.md](GOVERNANCE.md), Review), and the
   change is then reviewed afterwards. Since 13 October 2026 nobody in the project can approve a batch, which is
   the owner's own work, so every landing is made that way until somebody can ([GOVERNANCE.md](GOVERNANCE.md),
   Review).
2. **The Azure federated credential for that one tag**, made before the tag is pushed. Its subject names the
   project's path at that moment, `hmailserver/hmailserver` since the project moved into the group on 29 September
   2026 ([RELEASE.md](RELEASE.md), "On GitLab, from 6.3.5"). Without it, `release-authenticode` cannot sign the
   installer, and no later job runs.
3. **The release tag, signed with the one key in `.gitlab/allowed_signers`.** That is the owner's key, on his own
   machine. `release` refuses a tag signed by any other key.
4. **Starting `release`, and later `release-publish`, as the owner.** He is the only person among the project's
   members with the Maintainer role or above; the one other member listed with such a role is the bot user of
   the project access token `laptop-git`, an Owner, and the token is revoked, so nothing can act as it. The
   package protection rules let only an Owner upload a release's files.
5. **Removing the federated credential** once the release is published.

Two more things are needed besides:

- the project's runners must be up: bench150 and S1-WINBENCH, the two Windows benches, which share the Windows
  suite between them, and linix, server2 or s1-linux, which share the tag `linix`, and for the batch's own
  pipeline linix's entry `linix-systemd` and server2, the one runner with the tag `kvm`, as well
  ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "The runners");
- the installer smoke test is run by hand between `release-sign` and `release-publish`.

If the Azure side cannot sign, the pipeline cannot publish a release. This could happen if the identity
validation has lapsed and cannot be renewed in time. The fallback is [a release made by
hand](RELEASE.md#a-release-made-by-hand), as 6.3.4 was: its sums are signed with the tag's key, and it carries no
Sigstore bundle. No server installs such a release by live update. Each installation is upgraded by hand.

### What must stay valid for a security release

What a release depends on that expires, as recorded on 29 September 2026, when the GitLab tokens' dates were read
through GitLab's API, and read again on 14 October 2026, when the tokens, the project's CI/CD variables (by name)
and its members were read through the API and the exceptions file was counted, as it was again on 18 October 2026;
the Azure rows are as recorded on the first day, S1-WINBENCH's licence as read on that bench on 15 October 2026, and
bench150's from the day it was installed. No value of any of them is in the repository, and none is written here.

| What | Used for | Expires |
|---|---|---|
| The release-tag signing key, the one key in `.gitlab/allowed_signers` | Signing each release tag, on the owner's machine | An SSH key has no expiry. A lost key is replaced by putting its successor on `master` ([GOVERNANCE.md](GOVERNANCE.md#critical-assets)) |
| The Azure federated credential | `release-authenticode`'s exchange of the job's ID token | It does not expire. It is made for one tag and removed after that release. An app registration holds twenty at most |
| Azure Artifact Signing's identity validation | Every Authenticode signature | It must be renewed, and signing stops if it lapses. **The date is not recorded here**: it is read in the Azure portal (the Artifact Signing account, *Identity validations*), which has no API for it |
| The Azure subscription (pay-as-you-go) | The Artifact Signing account | It does not expire, but it must be kept paid |
| The app registration's client secret, made for the GitHub workflow on 10 September 2026 | Nothing since the move to GitLab: the pipeline needs no secret | 10 September 2028. It should be deleted (`az ad app credential delete`), not left to run out |
| `.gitlab/security-exceptions.yml`, 491 accepted findings | The release job's security gate | Each finding has its own date: 278 expire on 26 March 2027, 12 on 27 March 2027, 157 on 25 September 2027, 2 on 27 September 2027, 21 on 1 October 2027, 6 on 2 October 2027, 10 on 4 October 2027, 3 on 12 October 2027 and 2 on 15 October 2027. After its date, a finding holds every release until it has been read again and given a new expiry |
| bench150's Windows licence: Windows Server 2025 Standard Evaluation, installed on 16 September 2026 | The Windows bench of every release: half of the Windows suite (`windows-gate`), `windows-dotnet`, and the jobs no other machine of the project's can run - `windows-installer`, `release-authenticode` and the Windows package jobs | 180 days from its activation: 15 March 2027 at the earliest (`slmgr /xpr` on the bench gives the day, and `slmgr /dlv` the rearms left). A rearm, if one is left, gives 180 days from the day it is taken, so that taken before the licence ends it ends before 30 September 2027. Before then, a licensed key (`DISM /Online /Set-Edition`) or a rebuild ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "bench150") |
| S1-WINBENCH's Windows licence: Windows Server 2025 Standard Evaluation, activated on 14 October 2026 | The other half of the Windows suite (`windows-gate-s1vm`), which `windows-installer`, the Windows package jobs and `release` wait for | 12 April 2027. One rearm is left: taken between 4 and 11 April 2027, it carries the bench past 30 September 2027. After that, a licensed key or a rebuild. While S1-WINBENCH is down, bench150 can take its half, by a runner tag and no commit ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "S1-WINBENCH") |
| `RENOVATE_TOKEN`, the owner's personal access token `renovate` | Renovate's merge requests | 27 September 2027 |
| `sonarcloud-hmailserver`, the owner's personal access token | SonarQube Cloud's binding to the GitLab group | 27 September 2027 |
| `laptop-git`, a project access token with the Owner role and the `api` scope | Nothing: it is revoked, and was last used on 29 September 2026. It was made for work from the maintainers' machine, and no CI job used it: the jobs use their own `CI_JOB_TOKEN`. Its bot user is still listed among the project's members with the Owner role, and nothing can act as it | Revoked. It would have expired on 27 September 2027 |
| `glab-laptop`, the owner's personal access token | `glab` on the maintainers' machine, which `build/land-via-mr.py` needs | 26 September 2027 |
| `GITLAB_AUTH_TOKEN`, the owner's personal access token for Scorecard | The weekly Scorecard job | Not made yet (the variable does not exist). A year from when it is made ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "OpenSSF Scorecard") |
| `HM_EXPORT_TOKEN`, and `HM_EXPORT_FULL_TOKEN` if set | The weekly export ([Mirroring.md](hmailserver/docs/Mirroring.md)) | Not made yet. The date is chosen when a token is made. The job names it from 30 days before |
| `HM_REPO_SIGNING_KEY`, the apt and dnf repositories' signing subkey | `package-repos` | Not made yet. Five years from when it is made. The job warns from 180 days before ([PackageManagers.md](hmailserver/docs/PackageManagers.md)) |
| The release channels' credentials (`PSGALLERY_API_KEY`, `SNAPCRAFT_STORE_CREDENTIALS`, `DOCKERHUB_TOKEN`, `QUAY_ROBOT_TOKEN`, `AUR_SSH_KEY`, `OBS_RUNSERVICE_TOKEN`, `LAUNCHPAD_SIGNING_KEY`, `COPR_WEBHOOK_URL`) | Publishing a release to its channels, not making it ([Channels.md](hmailserver/docs/Channels.md)) | `SNAPCRAFT_STORE_CREDENTIALS` alone is made (6 October 2026), and expires on 6 October 2027. The rest are not made yet: the PowerShell Gallery's key lasts 365 days at most, the Copr webhook does not expire, and the others expire as set when made |
| `CI_JOB_TOKEN` and the jobs' ID tokens (Sigstore's, Azure's, PyPI's) | The release jobs | Each lives for its job. Nothing to renew |

A token is replaced before its date, not on it, and the table is updated when it is. The maintainers keep the
dates in a calendar as well.

## Scope

hMailServer is a network-facing mail server (SMTP/IMAP/POP3 plus optional REST API, metrics and web-services
listeners). Reports of particular interest:

- Remote code execution or memory corruption in protocol handlers
- Authentication or authorization bypass
- TLS/crypto weaknesses (DANE, MTA-STS, DKIM/ARC, certificate handling)
- SQL injection in the persistence layer
- Privilege escalation via the Windows service or COM API

### Out of scope

These are known properties of the design rather than vulnerabilities, and a report about them will be closed with
a pointer back here:

- Findings that require an account that is already a server administrator. Administrators can run scripts and
  change the configuration by design.
- Missing hardening on a listener the operator deliberately exposed without TLS. The server lets you do that; it
  warns you first.
- Output from an automated scanner with no demonstrated impact, including version-banner findings and
  TLS-configuration grades.
- Vulnerabilities in a third-party dependency with no path from hMailServer to the affected code. Report those
  upstream; tell us as well if we ship a vulnerable version, and see
  [ThirdPartyBinaries.md](hmailserver/docs/ThirdPartyBinaries.md) for what we ship and where it came from.

## Verifying a release

Releases are at <https://gitlab.com/hmailserver/hmailserver/-/releases>, and since 18 October 2026 at
<https://github.com/RainbowWizard-coder/hMailServer/releases> as well. Every release up to and including 6.3.3 was
first published on GitHub, under the old account `Progressiverobot`. Where GitLab's releases page carries a file
from one of them, it is the same file, byte for byte, with the same signatures and Sigstore bundle, so everything
below verifies it exactly as it verified the original. GitHub's releases page, since 18 October 2026, is a copy of
GitLab's: a release is built, signed and published on GitLab first and then copied there, and every release from
6.0.0-B1 to 6.3.7 is there with the same files, byte for byte, each checked against the SHA-256 that GitLab's
package registry records for it; so everything below verifies a file from either page the same way. GitHub writes
"." where a file name has "~", so the per-distribution `.deb` files are renamed there, their bytes the same; the
notes of 6.3.6 and 6.3.7 are cut at GitHub's limit of 125,000 characters and attached whole as a file; and GitHub's
immutable-releases setting is on: a release published there with it on keeps its files and its tag as published,
though its notes can still be edited, and so do the copies of 6.3.6 and 6.3.7, published after it was switched on
during the copying of 18 October 2026 (at 05:26 and 05:31 UTC), while the copies of the releases before them,
6.0.0-B1 to 6.3.5 and the two build-inputs releases, published before it, can still be changed.

- **The release tag** (from `v6.2.23-alpha2` on) is an annotated tag signed with a maintainer's SSH key listed in
  `.gitlab/allowed_signers`. That allow list is in the repository, so a clone can check it without trusting
  anything else:

  ```
  git -c gpg.ssh.allowedSignersFile=.gitlab/allowed_signers verify-tag v6.2.23-alpha2
  ```

- **The signed sums of a release built by hand** (from 6.3.4 on). Such a release carries
  `hmailserver-<v>-SHA256SUMS.txt`, which names every file of the release but itself and its `.sig` with its
  SHA-256, and
  `hmailserver-<v>-SHA256SUMS.txt.sig`, an SSH signature of it made with the key that signs the release tags - the
  one key in `.gitlab/allowed_signers`, under the principal `christopher.j.holloway@outlook.com`. With OpenSSH 8.1
  or later, in a clone (for `.gitlab/allowed_signers`) and with the downloads beside you:

  ```
  ssh-keygen -Y verify -f .gitlab/allowed_signers -I christopher.j.holloway@outlook.com -n file -s hmailserver-<v>-SHA256SUMS.txt.sig < hmailserver-<v>-SHA256SUMS.txt
  sha256sum -c hmailserver-<v>-SHA256SUMS.txt
  ```

  The first must say `Good "file" signature for christopher.j.holloway@outlook.com`, and the second `OK` for every
  file (`--ignore-missing` when you downloaded only some). A release made by the GitLab pipeline, from 6.3.5, has
  no `.sig`: the pipeline cannot hold that key, so its files are signed by Sigstore instead, as below.

- **The assets of every release from 6.2.19 on** (the withdrawn 6.2.22-pre4 and the superseded -pre5 aside,
  6.2.22-pre3's two SBOMs, which carry none, and 6.3.4, built by hand, below) carry Sigstore bundles
  (`<asset>.cosign.bundle`), keyless and recorded in the public Rekor log. For the releases up to
  6.3.3 the signing certificate names this repository's GitHub workflow, because that is where they were signed, so
  the identity and the issuer below are facts about those files and stay right for them wherever they are hosted.
  The bundle carries the certificate and the log's proof, and nothing in the check contacts GitHub. With
  [cosign](https://docs.sigstore.dev/cosign/) installed:

  ```
  cosign verify-blob \
    --bundle hMailServer-x.y.z-x64.exe.cosign.bundle \
    --certificate-identity-regexp '^https://github\.com/Progressiverobot/hmailserver/' \
    --certificate-oidc-issuer https://token.actions.githubusercontent.com \
    hMailServer-x.y.z-x64.exe
  ```

  Only GitHub Actions can issue that identity, so no later release can carry it. 6.3.4, built and signed by hand,
  carries no bundle. From 6.3.5 the release jobs of `.gitlab-ci.yml` sign each asset in the tag's own pipeline,
  with the issuer `https://gitlab.com`. From 6.3.6 the identity is
  `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`, with the project's id,
  `86730559`, in the certificate: the project moved into the group [`hmailserver`](https://gitlab.com/hmailserver)
  on 29 September 2026. 6.3.5, signed that day before the move, carries the identity
  `https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v6.3.5`, the path the project began at,
  and the same id. A server from 6.3.4 on trusts both paths, the group's only with that id, whatever its update
  settings say ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)). Check a file of a release from 6.3.6 on with
  cosign, and then the id with OpenSSL:

  ```
  cosign verify-blob \
    --bundle hMailServer-x.y.z-x64.exe.cosign.bundle \
    --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/vx.y.z \
    --certificate-oidc-issuer https://gitlab.com \
    hMailServer-x.y.z-x64.exe
  jq -r .verificationMaterial.certificate.rawBytes hMailServer-x.y.z-x64.exe.cosign.bundle \
    | base64 -d | openssl x509 -inform DER -noout -text | grep -A1 1.3.6.1.4.1.57264.1.15
  ```

  The first must say `Verified OK`, and the second print `..86730559` under the extension's name (the two dots
  are the value's DER tag and length). cosign checks the identity, which is a path, and a path could pass to another
  project were the group or the project renamed or deleted; the id is the project's own, GitLab gives it to no
  other, and cosign does not check it. OpenSSL reads it from the certificate's extension (Fulcio's source repository
  identifier, `1.3.6.1.4.1.57264.1.15`), not by a search of the certificate's bytes, which a public key holding the
  same digits could fool. From a checkout or the source tarball, `python3 build/ci/sigstore-certificate.py
  repository-id <bundle>` reads the extension the way the server does and prints `id=86730559`. The same check
  reads the provenance's bundle below, and ContainerImage.md, "Checking an image", gives it for the container image.

  From 6.3.6 each file but a compiled file's SBOM also carries its bundle a second time, byte for byte, as
  `<file>.sigstore.json`, the name Sigstore's own clients and OpenSSF Scorecard look for; either name verifies the
  same way. A pipeline of a `batch*` branch signs a rehearsal of the release with the identity
  `.../.gitlab-ci.yml@refs/heads/batch<...>`; those files are never published, and no server installs them.

- **Build provenance** (from 6.3.6). `hmailserver-<version>.intoto.sigstore.json` on the release page is the
  release's SLSA v1 build provenance: an in-toto statement that names every file of the release but the Sigstore
  bundles by its SHA-256 - the installers, the PowerShell module's two files, every package, the AppImages, the source
  of the libraries linked into their runtime and of the libraries they carry, the source tarball, the chart, the sums
  and every SBOM; not a file's `.cosign.bundle` or `.sigstore.json`, nor the provenance itself or its `.intoto.jsonl`
  (below) - with the commit it was built from and the tag's pipeline as the builder, signed by that pipeline with
  Sigstore (a bundle holding a DSSE envelope, recorded in the public Rekor log). cosign is the verifier; the SLSA
  project's `slsa-verifier` is not needed. To check that a download is one the tag's pipeline built:

  ```
  cosign verify-blob-attestation --new-bundle-format \
    --bundle hmailserver-x.y.z.intoto.sigstore.json \
    --type slsaprovenance1 \
    --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/vx.y.z \
    --certificate-oidc-issuer https://gitlab.com \
    hmailserver_x.y.z_amd64.deb
  ```

  It says `Verified OK` only when the file's SHA-256 is one the statement names and the statement was signed by that
  identity, at the group's path, where every release that carries a provenance (from 6.3.6) is signed. To read
  the statement itself: `jq -r .dsseEnvelope.payload hmailserver-x.y.z.intoto.sigstore.json | base64 -d | jq .`.
  The file is named for what it is, a Sigstore bundle (`.sigstore.json`, the name Sigstore's clients take).

  From the release after 6.3.7 the release also carries `hmailserver-<version>.intoto.jsonl`: the bundle's DSSE
  envelope alone - the statement and its one signature, each as the bundle has it - as one line of JSON. That is the
  form in-toto's attestation specification gives a bundle of attestations, JSON Lines with one DSSE envelope a line,
  in a file whose name ends in `.intoto.jsonl` - the name the SLSA generator's provenance of 6.3.2 and 6.3.3 also
  had, though those two files are Sigstore bundles under it. It is not a second signature: the release pipeline
  writes it from the Sigstore bundle after signing, and holds it to the bundle byte for byte before the release is
  published. It carries neither the certificate that verifies its signature nor the transparency log's entry, which
  only the bundle carries - the log's dsse entry, in the bundle, records that signature with the certificate and the
  SHA-256 of the statement - so verify with the bundle, as above, and read the line as the same envelope:
  `jq -c .dsseEnvelope hmailserver-x.y.z.intoto.sigstore.json` and `jq -c . hmailserver-x.y.z.intoto.jsonl` print
  the same line. It is there because OpenSSF Scorecard's Signed-Releases check, which reads a file's name alone,
  counts a release's provenance only from a file whose name ends in `.intoto.jsonl`, and counts the bundle as a
  signature; 6.3.6 and 6.3.7, the releases that carry the bundle, carry it alone.

- **Authenticode.** Since 6.3.1 the Windows installer is also Authenticode-signed, and from 6.3.6 the PowerShell
  module's `HmailServer.psd1` and `HmailServer.psm1` in the release; the Linux packages are not.
  Neither signature above replaces it: Windows SmartScreen and the UAC prompt read only Authenticode, and
  PowerShell's AllSigned policy reads it for a module. In PowerShell,
  `Get-AuthenticodeSignature .\hMailServer-x.y.z-x64.exe` shows it.

## Supply chain

The third-party executables and libraries the build uses are not committed to this repository. They are
inventoried in [hmailserver/docs/ThirdPartyBinaries.md](hmailserver/docs/ThirdPartyBinaries.md): those fetched
from one of the project's `build-inputs-<n>` releases are checked against a recorded SHA-256 whenever
`build/get-installer-binaries.ps1` places them, the MSVC runtime is checked by version, and the ADO type
libraries, which are Windows' own files, are checked only for presence, with their hash reported. The
script downloads those releases' files from the project's package registry on GitLab: `build-inputs-1`, where the
files committed before 11 September 2026 were re-published byte for byte after GitHub suspended the project's
account, and `build-inputs-2`, the MariaDB Connector/C 3.4.11 client this project builds from MariaDB's source
([CONTRIBUTING.md](CONTRIBUTING.md#prerequisites)).

OpenSSL, Boost and libpq are built from source by `libraries/build-*.ps1`, at the versions `hMailServer.vcxproj`
pins, and each script checks the SHA-256 of the source archive it downloads.

Releases from 6.2.3 on carry an SBOM, in SPDX and in CycloneDX (the withdrawn pre-release 6.2.22-pre4, and -pre5,
which -pre6 superseded, aside). Up to 6.3.3 they were made by the GitHub workflow `sbom.yml` (in the git history at
`v6.3.3`), which added the native libraries to the .NET dependency graph
(`build/merge-native-dependencies-into-sbom.ps1`); on GitLab the `linux-sbom` job makes them.

The five GitLab releases before 6.2.3 that carry an installer - `v6.0.0-B1`, `v6.0.0`, `v6.1.0`, `v6.2.1` and
`v6.2.2` - carry `<installer>.cdx.json` too, attached on 26 September 2026 and made afterwards from each installer
itself: innoextract 1.9 unpacked it and Syft 1.52.0 read what came out, every file listed by its SHA-256. Each was
checked by downloading it anonymously from its release page.

### Software bills of materials

From 6.3.4 on, every compiled file a release carries goes out with a software bill of materials of its own, in
CycloneDX 1.6, beside it on the release page and named after it:

| The file | Its SBOM | What the SBOM lists |
|---|---|---|
| `hMailServer-<version>-x64.exe`, the Windows installer | `hMailServer-<version>-x64.exe.cdx.json` | OpenSSL, Boost and libpq, which the server is linked against, and the zlib compiled into it, at the versions `hMailServer.vcxproj` builds with; every third-party file the installer packs (the .NET runtime, the MSVC runtime, 7-Zip, the MariaDB client, SQL Server Compact), each file by the SHA-256 [third-party-binaries.json](hmailserver/docs/third-party-binaries.json) pins it by; and every NuGet package of the tools it installs, at the version and with the SHA-512 their `packages.lock.json` resolves |
| from 6.3.6, `hMailServer-<version>-arm64.exe`, the Windows installer for ARM64 | `hMailServer-<version>-arm64.exe.cdx.json` | The same for the ARM64 build: the ARM64 OpenSSL, Boost and libpq and the zlib compiled in, at the versions `hMailServer.vcxproj`'s ARM64 configuration builds with, and every third-party file `hMailServerARM64.iss` packs (the ARM64 .NET runtime, the ARM64 MSVC runtime, 7-Zip's ARM64 console), from `build/native-dependencies.ps1 -Platform ARM64`'s list; and every NuGet package of the tools it packs, as their `packages.lock.json` resolves it; made from the installer as Authenticode signed it |
| `hmailserver_<version>_amd64.deb`, `hmailserver-<version>-1.x86_64.rpm`, and from 6.3.6 `hmailserver_<version>_arm64.deb` and `hmailserver-<version>-1.aarch64.rpm` | `<package>.cdx.json` | The libraries the Linux server needs from the distribution (glibc, libstdc++, OpenSSL, zlib, libpq, OpenLDAP), each with the development package and version it was built against; Boost, linked into the binary, at its exact version; the QuickJS compiled into it; and what the package itself declares it depends on |
| from 6.3.6, each distribution release's own package: `hmailserver_<version>-1~<release>_<arch>.deb` for Debian 12 and 13 and Ubuntu 24.04, `hmailserver-<version>-1.<release>.<arch>.rpm` for Fedora 44 and 43, EL 10 and 9, openSUSE Leap 16.0 and Tumbleweed | `<package>.cdx.json` | The same as the generic packages', from what that release's build container recorded; where the job built Boost from boost.org's release tarball (Debian 12, EL, openSUSE) the SBOM says so |
| from 6.3.6, `hMailServer-<version>-x86_64.AppImage` and `hMailServer-<version>-aarch64.AppImage` | `<image>.cdx.json` | The server's libraries as for a package; every library the image carries in `usr/lib`, by the Ubuntu 24.04 package and version its file came from, that file's SHA-256 (the copy in the image has a RUNPATH of its own) and the source package and version it was built from; and every Alpine package whose files the runtime in front of the image links statically, at its version, with its aports commit, its licence and each file linked |
| from 6.3.6, the source tarball `hmailserver-<version>.tar.gz` | `hmailserver-<version>.tar.gz.cdx.json` | The third-party code and data it holds, as [SourceTarball.md](hmailserver/docs/SourceTarball.md) declares them, each held to the tarball, QuickJS at the version its `VERSION` file gives |
| the container image, once one is published | an attestation on each platform's image, linux/amd64 and linux/arm64 (predicate type `cyclonedx`) | Every distribution package in that platform's image, as Syft reads it, and what that architecture's `.deb`'s own SBOM names inside the binary; the image by its digest and its platform |
| the source tree | `hmailserver.spdx.json`, `hmailserver.cyclonedx.json` | Syft's inventory of the tree, with the native libraries it cannot see added |

From 6.3.6 a release also carries `hmailserver-<version>-appimage-runtime-sources.tar.gz`, the source of the libraries
linked into the AppImages' runtime as Alpine built them, which the LGPL-2.1 of libfuse3 and the GPL of lzo ask to go
with the binary. It is source, not a compiled file, and has no SBOM: its `README.txt` names each library, its version,
licence and aports commit and where each file came from. Each source file of a library is held to the SHA-512 its
APKBUILD, Alpine's build script, gives it, and each APKBUILD and patch to the aports commit its package records
([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md), "Licences").

From 6.3.6 a release also carries `hmailserver-<version>-appimage-bundled-sources.tar.gz`, the source of every shared
library the AppImages carry in `usr/lib`, as Ubuntu built them: each source package (its `.dsc`, upstream archives and
Ubuntu's changes) at the version the image's package was built from, which the LGPL of GnuTLS, Nettle, GMP, libidn2,
libunistring, libtasn1 and keyutils asks to be offered with the binary. It is source and has no SBOM: its `README.txt`
names each source package, its version and licence, the libraries each image took from it, and each file's SHA-256.
Each file came from Ubuntu's archive, or its snapshot service for a version the archive has dropped, and is held to
the SHA-256 of a `Sources` index whose `InRelease` verified against Ubuntu's archive keys, and again to its `.dsc`.
Each AppImage lists those libraries, their packages and source packages in
`usr/share/hmailserver/licenses/bundled.txt`, which names the file, beside each package's copyright file and
hMailServer's own AGPL-3.0 text ([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md), "Licences").

Each document's subject (`metadata.component`) is its file, by SHA-256 and SHA-512, so it cannot be mistaken for
another file's. `build/asset-sbom.py` makes them: the release jobs of `.gitlab-ci.yml` make the x86-64 packages' in
`linux-build`, where the build ran; the generic AArch64 packages' are made by `deb-ubuntu26.04-arm64-sbom` and
`rpm-fedora44-aarch64-sbom` from what each build recorded, in its distribution's own container, that it was built
against; every other package's, the AppImages' and the tarball's by `release-packages` (`.gitlab/ci/release-packages.yml`)
the same way, the AppImages' from what `appimage-build.sh` recorded; the installer's in `release-sbom`, from the bytes
Authenticode signed. The Helm chart, which compiles nothing and names only the release's image, has none. The release
jobs refuse to publish a release whose compiled file has no SBOM or one that describes other bytes. The apt and dnf
repositories serve the release's own packages, so their SBOMs are those on the release page.

To check one, download the file and its `.cdx.json` from the release page, then:

1. **That it is the project's.** From 6.3.5 the SBOM carries a Sigstore bundle like every other file
   (`<file>.cdx.json.cosign.bundle`); check it with the `cosign verify-blob` command in *Verifying a release*
   above. For 6.3.4, the release's `SHA256SUMS` file lists the SBOMs' SHA-256 with the rest.
2. **That it describes your file.** From a clone of the repository at the release's tag:

   ```
   python3 build/asset-sbom.py verify hMailServer-x.y.z-x64.exe.cdx.json hMailServer-x.y.z-x64.exe
   ```

   checks that the document is well formed and that its subject's SHA-256 is the file's, and lists the components.
   Without the repository, compare the two by hand:

   ```
   jq -r '.metadata.component.hashes[] | select(.alg == "SHA-256") | .content' hmailserver_x.y.z_amd64.deb.cdx.json
   sha256sum hmailserver_x.y.z_amd64.deb
   ```

3. **What is in it.** `jq -r '.components[] | [.name, (.version // "-"), .purl] | @tsv' <file>.cdx.json`, or any
   tool that reads CycloneDX, such as a vulnerability scanner or Dependency-Track.

For the container image, once one is published ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md),
"The container image"), each platform's SBOM is an attestation stored beside that platform's image in the registry,
signed by the same pipeline identity as its signature. A tag names the index over both platforms' images;
`docker manifest inspect` of it lists each image's digest:

```
cosign verify-attestation --type cyclonedx \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  registry.gitlab.com/hmailserver/hmailserver@sha256:<the platform's digest> | jq -r .payload | base64 -d | jq .predicate
```

This checks the attestation by its identity, a path; `cosign verify-attestation` neither checks the project's id nor
writes out the certificate it read. Read the id from the certificates of the image's own signatures, as
[ContainerImage.md](hmailserver/docs/ContainerImage.md), "Checking an image", shows: an image whose signatures carry
`86730559` is this project's, and the channels' registry mirror holds each attestation's certificate to the same id.

What an SBOM cannot say: the Linux libraries other than Boost are the distribution's, found at run time, so their
version is whatever the machine has installed; the SBOM names the one the package was built against and the range
the package requires.

## For a security questionnaire

A buyer's security or procurement team will find the answers a vendor questionnaire asks for - where mail and
personal data are kept and what leaves the server, ISO/IEC 27001 and SOC 2 control mappings of the product, the
supply chain in one place, the cryptography with its defaults, and what is not in place - in
[hmailserver/docs/EvidencePack.md](hmailserver/docs/EvidencePack.md), each with the file it can be checked in. It
is not an audit report: there is none.

The same facts for a tool - how to report a vulnerability, the policies, the release's signatures, provenance and SBOMs,
the security tools that run on the code and where, and the self-assessment - are in [SECURITY-INSIGHTS.yml](SECURITY-INSIGHTS.yml),
in the OpenSSF Security Insights format, version 2.2.0, which OpenSSF's OSPS Baseline scanner reads; `build/check-security-insights.py`
holds it to that format's schema and to this tree.

## Vulnerability management policy

What is checked, on what, and what happens when it finds something. A vulnerability meets two evaluations before it
reaches a user: **every change** is judged on what it adds before it can reach `master` ([Every
change](#every-change)), and **every release** on its whole tree before anything is signed ([Before a
release](#before-a-release)). Each passes a finding it counts only when the finding is fixed, or when a declaration
already on the target branch says why it is not exploitable ([Declaring a finding not
exploitable](#declaring-a-finding-not-exploitable)). On GitHub, until 18 September 2026, CodeQL, dependency review and
Dependabot did this work; what replaced each on GitLab, and where every tool comes from, is in
[hmailserver/docs/ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md#security-scanning), "Security
scanning".

### Every change

- **The route.** A change reaches `master` only through a merge request, merged by fast-forward, and GitLab merges one
  only on a pipeline that succeeded (*Pipelines must succeed*, a project setting). For a merge request from Renovate
  or from any branch of this project that is not a protected `batch*` one, that is the merge request's own pipeline;
  for a maintainer's `batch*` branch, whose merge request runs no pipeline of its own, it is the branch's pipeline,
  which GitLab holds the merge request to. A merge request from a fork runs its own pipeline in the fork, with the
  fork's CI files and on runners the fork's owner chooses, so what that pipeline says is the fork's to report, not a
  gate: such a change lands through a batch - a maintainer takes its commits into a `batch*` branch, whose pipeline
  runs here - or on a pipeline a maintainer runs for the merge request in this project, once its CI files have been
  read. `build/land-via-mr.py`, which lands a batch, refuses a pipeline that was skipped, one another project ran, one
  in which a gate below did not succeed itself - left out by a rule, skipped, or failed while allowed to fail - and
  one whose gates did not judge the batch against `master`'s tip, as each says in its log
  ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md#landing-on-master), "Landing on master"). Two
  project settings, the owner's, take away the ways round it: `master` takes a push from no one (its protection's
  *Allowed to push and merge* is *No one*, *Allowed to merge* is Maintainers, and force push is off), and a skipped
  pipeline does not count as one that succeeded (`allow_merge_on_skipped_pipeline` false, which is how GitLab already
  treated one here). The owner sets them once the batch that brings these gates has landed through them. It landed
  on 9 October 2026 (`batch44`), and on 14 October 2026 the second held and the first was still to be set; until it
  is, a Maintainer can still push to `master` directly, which this policy forbids and nothing checks. The owner can
  always unprotect `master`: that is a break-glass step, and one taken is said, with its reason, in the next
  release's notes.
- **Dependencies:** `mr-dependency-check` (`build/ci/mr-dependency-check.py`), in every such pipeline.
  - *What it evaluates:* each NuGet version the change adds (`packages*.lock.json`, the ARM64 ones among them, and
    `packages.config`), each PyPI version it adds (`*requirements*.txt`), and each native or vendored library whose
    pinned version it changes: OpenSSL, Boost and libpq (`hMailServer.vcxproj`), zlib and QuickJS (vendored), and
    MariaDB Connector/C, 7-Zip, SQL Server Compact and the .NET Desktop Runtime each installer carries
    (`hmailserver/docs/third-party-binaries.json`). A NuGet or PyPI version is looked up in OSV's own export of its
    ecosystem's advisories; a native library is asked of OSV by its upstream repository's tag, and the runtime by its
    NuGet runtime packages, beside a control version a published advisory is known to affect.
  - *A violation*, which fails the pipeline: an OSV `MAL-` record (the OpenSSF malicious-packages data), rated
    Critical; an advisory of severity Critical or High, with a fixed version or without one, rated by the GitHub
    advisory database's label, else its CVSS v3 vector, else its CVSS v4.0 vector, else its aliases'; such an
    advisory that may affect the version, where the versions cannot be ordered as the ecosystem orders them, a native
    version is older than its upstream repository's first tag, or one older than its control is named in no advisory
    while the control is (OSV did not match its tag: OpenSSL's 0.9.6 draws none where 4.0.0 draws 43); and a
    requirements line the change adds that pins no version by `name==version` - a range, a URL or a path, an
    editable, another index, a requirements file the tree does not hold, a `${...}` pip takes from the environment -
    since pip would install what OSV cannot be asked about.
  - *Not a violation*, and listed in the job's log: an advisory of severity Medium or Low, or with no rating; and
    UNKNOWN, a library for which OSV has no usable data (no repository it knows, as for SQL Server Compact, or nothing
    for a control OSV is not known to name, as for libpq and QuickJS), which is read by hand against the release's
    SBOM before a release ([Dependencies](#dependencies-software-composition-analysis), "Before a release").
  - An input that cannot be read - the base, a lock file, a pin, OSV, or a control OSV is known to name answering
    nothing - never passes: the job exits 2 and is retried. A base whose own files are broken holds no change by that
    alone, or no change could mend it: its lock file that cannot be read names nothing, and its exceptions file that
    cannot be read accepts nothing.
- **Weaknesses:** GitLab's *semgrep-sast*, and *semgrep-sast-gate* (`build/ci/security-gate.py --merge-request`), which
  reads its report, in every such pipeline.
  - *What it evaluates:* the C, C++, C#, Python and JavaScript of the tree, with GitLab's Semgrep analyzer and rules,
    less the paths `SAST_EXCLUDED_PATHS` names in `.gitlab/ci/gitlab-security.yml` (the tests, the vendored sources and
    the three string tables generated from documents, each entry held to the root), and less what the analyzer's own
    ignore template leaves out at any depth: every directory named `build`, `dist`, `vendor` or `node_modules`, and
    minified scripts. So the build and CI scripts under `build/` - the gate scripts among them - are read by the
    *semgrep* job alone, in a batch.
  - *A violation:* a finding of severity Critical or High in a file the change touches; and, when the change alters
    the scan's own configuration - edits, removes or moves `.gitlab-ci.yml`, `.gitlab/ci/gitlab-security.yml`,
    `.gitlab/ci/mr-checks.yml`, `.gitlab/sast-ruleset.toml`, `build/ci/security-gate.py` or
    `build/ci/security-findings.py` - one in any file. That widens what counts, and cannot see a finding the change
    keeps out of the report; so a change that **narrows what a scan reads** - a `.semgrepignore` at any depth, an
    exclusion that reaches a path the base's did not, the analyzer's options or any other `SAST_*`, `SEMGREP_*` or
    `SECURE_*` variable, GitLab's custom ruleset, or the ignore and suppression lists of the scans on the project's
    runners (`build/ci/semgrepignore`, `build/ci/cppcheck-suppressions.txt`, `build/ci/clang-tidy.yaml`,
    `HM_TIDY_CHECKS`): a line one of the lists gains, a `!` line Semgrep's list loses or puts before the line whose
    path it takes back, or a change of the lines of `static-analysis.sh` and `semgrep.sh` that choose what cppcheck,
    clang-tidy and Semgrep read and report (between their `# scan-scope begin` and `# scan-scope end` marks), or one
    of those tools' options put outside them - is a declaration, as an exception is, and lands alone: it is held while
    it changes anything else a scan reads. `static-analysis.sh` refuses `HM_TIDY_CHECKS` in a GitLab job, so that no
    project variable chooses its checks, and `semgrep.sh` refuses to run without its ignore list, without which
    Semgrep's own default would leave out `build/` and every test directory. A report that is missing, that GitLab's
    analyzer did not write in this pipeline (its `scan.analyzer.id`, and a start no earlier than the pipeline's
    creation), or whose scan did not finish, holds the change as well, and so does a tree that commits a scan's report
    or Semgrep's SARIF: *semgrep-sast* is not `allow_failure`, and removes any such file before its analyzer runs.
  - *In a batch's pipeline*, which the project's own runners serve, two scans more: *static-analysis* (cppcheck, and
    clang-tidy with the Clang static analyser) over the C++ the Linux build compiles, and *semgrep*, Semgrep's open
    rules over the C#, Python, JavaScript and shell. There a violation is any finding that is neither in the scan's
    committed baseline nor accepted by `master`'s `.gitlab/security-exceptions.yml`. They run unless every file the
    batch changes from `master` is a Markdown file at the root or under a directory whose name begins with a dot. A
    merge request from a fork or from an unprotected branch never reaches those runners: its code is read by
    *semgrep-sast* alone, until a maintainer takes it into a batch, whose pipeline then reads it.
  - *On every change, whatever the scans found:* an entry added to either baseline, a reason taken from one, or an
    inline suppression comment added to a file a scan reads (`nosemgrep`, `NOLINT` with `NOLINTNEXTLINE` and
    `NOLINTBEGIN`, or `cppcheck-suppress` in each form that begins one) is a violation of its own, which
    *semgrep-sast-gate* holds: it counts each file's comments at the change's base and at its head. So is a source
    file moved from where *semgrep-sast* reads it to where it does not - as git pairs a move, or deleted while a file
    of its name is added where the scan reads nothing, a move rewritten past what git pairs (a test of the same name
    as a source file the same change deletes is held with it, and lands apart or under a name of its own) - or added
    where only the analyzer's template leaves it unread (a `vendor/` below the root, say) and no exclusion of the
    project's names the place; and a file the change adds or alters that a rule gave up on - a time limit, memory or
    stack it ran out of - as Semgrep's SARIF beside the report says, which the analyzer leaves at a warning and calls a
    success. Where Semgrep's parser stumbled, which it reads on past (87 of the C++ and C# files on 6 October 2026, 18
    of them with findings all the same, and three of the Helm chart's templates), the notification is listed, not
    held. The gate reads the SARIF in the words of the analyzer it was checked against (6.26.1, Semgrep 1.174.0): a
    notification of a kind it does not know, or a time limit that names no file of the checkout as the analyzer names
    one (from the root, not as `./src/a.cpp` or by an absolute path, which would match no changed file), stops it as
    an input it cannot read (exit 2), never a pass, and its self-test fails while *semgrep-sast* pins another analyzer,
    until the new one's words are read (`build/ci/mirror-semgrep.sh`'s header).
  - *When the change alters the gates' own code* (`security-gate.py`, `security-findings.py`,
    `mr-dependency-check.py`, `dependency-scan.py`, `osv-offline-db.py`), the base's copy of each gate judges it too,
    over the change's tree, and the change passes only when both copies pass it. Whether that copy runs is read from
    the code that runs, against the base's, and no variable can stop it; each gate's log says how it ended, and
    `build/land-via-mr.py` lands such a batch only when both logs say the base's copy passed it. No file put beside a
    gate script runs as its code: each script of the gates and of the landing takes its own directory off Python's
    module path before it imports anything else, so a `build/ci/json.py` would not stand in for the `json` it
    imports (`build/check-release-chain.py`, `PATH_GUARD`). A batch that changes the gates' own files - their code;
    `.gitlab-ci.yml`, whose workflow, defaults and includes every job takes; the CI files the gate jobs are in or take
    a part from (`mr-checks.yml`, `gitlab-security.yml`, `no-github.yml`); `land-via-mr.py` and
    `check-release-chain.py`; `sign-off.py` and `scan-secrets.py`, which a landing runs, and
    `secret-detection-rules.sh`, which makes both secret scans' rules; and the source scans' scripts,
    `static-analysis.sh` and `semgrep.sh` - lands only with a person's word that the change was read whole
    (`--gates-read`), which the landing record keeps. No other CI file may define a job that holds a change (the
    gates, *semgrep-sast*, *static-analysis*, *semgrep*, *checkers*) or a template one reaches, since GitLab merges a
    second definition into the first, nor set what every job takes or include anything but GitLab's templates; and a
    landing needs *checkers*, which holds all of this, to have succeeded itself, not while allowed to fail.
- **What no gate reads.** Stated so that nobody takes it for covered; the owner may widen it. No gate reads
  PowerShell, VBScript, Inno Setup scripts, `.bat` files, SQL, or the Dockerfiles and the other files that describe
  infrastructure. The build and CI scripts under `build/` are read by no gate on a merge request: *semgrep-sast*'s
  analyzer leaves out every `build/` directory, and only a batch's *semgrep* job reads them. An edit to the vendored
  zlib or QuickJS sources is read by no gate: *semgrep-sast* leaves their directories out, *static-analysis* reads
  the server's own C++, and *mr-dependency-check* reads only their version strings, so such an edit is read by a
  person, as a change to the CI files is. The Windows-only C++ (the COM API, the service host, the ADO and SQL Server
  Compact backends), which only an MSVC build compiles, is read by *semgrep-sast*'s pattern rules alone. What a
  distribution supplies is not evaluated change by change: the build images of the per-distribution `.deb`, `.rpm`,
  Alpine (APK), Arch, Nix and snap packages, the libraries the AppImages carry, and the container image's base image.
  Those are pinned by digest or by lock and patched by their distributions; the CI image's packages are scanned by
  *dependency-scan* on a batch and before a release, and the release's container image by Trivy (*container_scanning*)
  once it is pushed, which holds nothing.
- **What the gates cannot hold**, said so that it is weighed. The gates hold against mistakes, not against a hostile
  change: a change's own CI files say which jobs run and what they run, and its own copies of the gate scripts run in
  them. The base's copies judge a change to the scripts too, and a landing asks for a person's word that it was read;
  but a change that edits the jobs themselves can leave a gate out or run another, and GitLab's Free tier cannot make
  a job required, so `build/land-via-mr.py` checks a batch's gate jobs and their base itself, and the owner, who alone
  merges, reads every change to the CI files. The owner can unprotect `master` (above); Semgrep's registry rules,
  which *semgrep* fetches, are not pinned; *semgrep-sast*'s rules have a time limit for each file, which a slower
  runner can reach where another did not (a finding then missing from a file the change does not touch passes the
  change, and holds the release when it is found again); an exception is keyed to the text of its line, so an edit
  elsewhere in the same function can make an accepted finding exploitable without changing its fingerprint; a source
  file moved where *semgrep-sast* does not read it under another name, or into a file already there, and rewritten
  past what git pairs as a move, reads as a deletion and a test like any other; a change
  to the scripts of the two scans on the project's runners (`static-analysis.sh`, `semgrep.sh`) outside the lines
  they mark as choosing what their tools read and report - a file taken away before a tool runs, an exit status
  taken for another, say - is held by no gate, only by the person whose word a landing asks for it (`--gates-read`),
  and those scans run the change's own `security-findings.py`, whose filters decide what a finding is, on the same
  word; and a project variable, which only the owner can set, reaches every job and stands above a job's own
  ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md#change-checks), "Change checks").

### Dependencies (software composition analysis)

- **What runs now.** On every change, *mr-dependency-check*, above ([Every change](#every-change)). Over the whole
  tree, the *dependency-scan* job of `.gitlab-ci.yml` runs Trivy over the twelve NuGet lock files, the three
  `packages.config` files and the scanners' own hashed Python requirements, and over the package database of the
  image the Linux server is built in, whose OpenSSL, libpq, libldap and Boost the Linux packages are built against.
  It asks OSV (`api.osv.dev`) about the libraries the Windows server is built with and no manifest names - OpenSSL,
  Boost, libpq, zlib, QuickJS, MariaDB Connector/C, 7-Zip - at the versions the build uses, and about the .NET Desktop
  Runtime each Windows installer carries, by its NuGet runtime packages at the release the installer pins; and it says
  UNKNOWN, not clean, for a library OSV holds no usable data on (libpq, QuickJS and SQL Server Compact, today). It
  honours the VEX document. An advisory against a header-only package of that image - `linux-libc-dev`, the kernel's
  user-space API headers, which carry no code of the kernel's, under which Trivy files every advisory against the
  kernel's source package - is not affected: the report lists it with that status, counted apart, and neither the scan
  nor the release job counts it among the critical or high with a fixed version (`HEADER_ONLY` in
  `build/ci/dependency-scan.py`, a list of that package alone today, with the reason; the rule reaches no other package
  and no other scan). It runs on the project's own runners for `master`, a release tag, and a `batch*` branch
  that changes more than documents from `master`. It passes whatever its scans find and fails only when one could
  not run, since GitLab takes no security report of a job that failed into the vulnerability report; its verdict
  fails *dependency-scan-verdict*, the job after it, on a high or critical advisory that has a fixed version. Both are
  `allow_failure`: a finding in the whole tree that no change brought must not stop an unrelated change, and it holds
  the release instead ([Before a release](#before-a-release)). Renovate (`renovate.json`, the `renovate` job, daily
  under its schedule) opens a merge request at once for every OSV advisory against a dependency in use, and a weekly
  one per ecosystem
  ([hmailserver/docs/ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "Dependency updates").
  What holds besides: the NuGet lock files are committed and the .NET projects are restored with `--locked-mode`, so
  a package cannot change under a build without a reviewed commit; the native libraries are pinned by version and by
  source hash, as above; and every third-party binary is checked against
  `hmailserver/docs/third-party-binaries.json`.
- **What ran on GitHub.** GitHub's dependency review (the workflow `dependency-review.yml`, in the git history at
  `v6.3.3`) compared every
  pull request's dependency graph against the GitHub Advisory Database, including its malware advisories, as a
  required check: a change that added a dependency with a known vulnerability of severity high or critical, or a
  package flagged as malicious, could not be merged. Dependabot (`dependabot.yml`, likewise) opened a pull request
  for every advisory touching a dependency in use, and for every new version of a GitHub Action the workflows pin.
  Neither runs on GitLab: *mr-dependency-check* stands for the dependency review, against OSV's data rather than
  GitHub's service, *dependency-scan* for the alerts, and Renovate for the update pull requests.
- **Remediation thresholds.** A high or critical advisory in a dependency is fixed, or the dependency replaced,
  **before the next release** and within 14 days of the advisory; moderate within 30 days; low within 90 days or
  with the next dependency refresh. A licence finding (a dependency whose licence is not compatible with
  AGPL-3.0-or-later) is treated as high.
- **Before a release.** A release is not cut while a known advisory of high or critical severity against a
  shipped dependency is open. The release job refuses one that has a fixed version
  ([Before a release](#before-a-release)); one with no fixed version yet is not refused by the job, and is read
  from the report by hand before the release: the dependency replaced, the advisory declared not affecting the
  project in the VEX document with its justification, or the release waits. The *dependency-scan* report of the
  release's commit is read first, and every
  component it marks UNKNOWN, with any the SBOM lists and the scan does not, is checked by hand against the
  components the release's SBOM lists (`hmailserver.spdx.json`, `hmailserver.cyclonedx.json`), which are also
  what a reader checks the shipped versions against.
- **Not affected.** A finding in a component that does not affect this project is declared so in the VEX document
  ([Declaring a finding not exploitable](#declaring-a-finding-not-exploitable)).

### Source code (static analysis)

- **What runs now.** On every change, GitLab's *semgrep-sast*, held by *semgrep-sast-gate* ([Every
  change](#every-change)). The C++ server is compiled with the toolchain's warnings as errors (`/WX`), the regression
  suite runs the server under its crash oracle before every release, first on a build that keeps every internal
  assertion (RELEASE.md step 8b), and the fuzz harnesses under `fuzz/` run for a set time on the release source
  of every minor release (step 8c). `build\analyze.ps1` runs MSVC's `/analyze` by hand. In `.gitlab-ci.yml`,
  *static-analysis* runs cppcheck and clang-tidy (the Clang static analyser among its checks) over every
  translation unit the Linux build compiles, and *semgrep* runs Semgrep's open rules over the C#, Python,
  JavaScript and shell sources, the Control Deck's and the webmail's scripts among them, for `master`, a `batch*`
  branch and a release tag. Each holds its findings to a committed baseline (`build/ci/static-analysis-baseline.txt`,
  `build/ci/semgrep-baseline.txt`) and to `master`'s exceptions file, so that it fails on a new finding that no
  exception accepts, and fails the pipeline then (since 6.3.6; before, both were allow_failure while their baselines
  were triaged). Neither reads the
  Windows-only sources (the COM API, the service host, the ADO and SQL Server Compact backends), which only an
  MSVC build compiles.
- **What ran on GitHub.** CodeQL (the workflow `codeql.yml` and its configuration, in the git history at `v6.3.3`) analysed the
  repository on every pull request and every push to `master`, and the `master` ruleset required its results. It
  does not run on GitLab: its command-line tool and its query packs are published only on GitHub. Since
  18 October 2026 CodeQL analyses the repository there on each push to `master`, on each pull request and weekly -
  GitHub's default setup at first, and then `.github/workflows/codeql.yml`, the same languages, queries and categories
  from a workflow of the project's, which OpenSSF Scorecard's SAST check counts where it did not count the default
  setup; the maintainers read its findings, and it holds no change and no release.
- **Remediation thresholds.** An error-level finding, or a security finding of high or critical severity, is
  fixed before merge, or declared not exploitable before the change that needs it. A medium finding is fixed within
  30 days; a low or note-level one with the next change to that file, or declared not exploitable with its reason. A
  release is not cut with an open error-level finding.
- **Suppressions.** There is one way to dismiss a finding, a declaration on the target branch, and baseline entries
  and inline comments are not it ([Declaring a finding not exploitable](#declaring-a-finding-not-exploitable)).

### Declaring a finding not exploitable

A finding that is not fixed passes a gate only through a declaration that it is not exploitable, and only through one
that is already on the target branch when the gate reads it: the change's base for the two change gates (where a
merge request's diff begins, or where a batch leaves `master`), and `master` for the source scans on the project's
runners and for the release, whose tagged commit is on `master`. A change's own copy of a declaration counts for no
finding in what the change touches, so no change can declare its own finding; the one case where it counts at all is
under *Order*, below.

- **Two documents.**
  - [`.gitlab/hmailserver.openvex.json`](.gitlab/hmailserver.openvex.json), an OpenVEX document, for a dependency: a
    statement that the project is `not_affected`, with its justification (the vulnerable code is not present, or is
    not reached, the feature is not built, the platform is not one the server runs on). It dismisses an advisory only
    where it names it, by its id or an alias, **and** names the component at the version found, by its purl
    (`pkg:nuget/`, `pkg:pypi/` with PEP 503's names, `pkg:generic/<name>` for a native library): a statement made for
    zlib 1.3.1 dismisses nothing for 1.3.2.
  - `.gitlab/security-exceptions.yml`, for a finding of any scan. An entry names the scan (*static-analysis*,
    *semgrep*, *semgrep-sast* or *dependency-scan*), the finding (the fingerprint the gate prints; for a dependency the
    advisory's id) and, for a dependency, its package; what it is; the reason it is **not exploitable** as the code
    stands, a false positive being one such reason; the GitLab user who reviewed it; the day; and an expiry no more
    than a year (366 days) after that day. After its expiry the finding holds again, a change and a release alike,
    until it is read again. Its header says how many findings it accepts and how each group of them was read.
- **Not a reason:** a fix that is waiting upstream. A finding that is exploitable is not declared: it holds, and the
  remediation thresholds above apply to it - it is fixed, its component replaced, or the release waits. None of the
  exceptions file's reasons is of that kind (read on 6 October 2026).
- **Who reviews.** The owner, who owns both files (`.gitlab/CODEOWNERS`) and is the one maintainer who merges: every
  declaration is his to accept, and its `reviewed-by` names him. Its merge request is approved as
  [GOVERNANCE.md](GOVERNANCE.md), "Review", says. A declaration read with an AI's help under the owner's delegation
  says so, in its entry or in the comment that heads its group.
- **Order.** A declaration lands first, in a change of its own, and the change that needs it after. The one exception
  is a change that re-pins *semgrep-sast*'s analyzer (its image), which answers for the whole tree: its own copy of
  the exceptions file may accept a finding in a file it does not touch, never in one it does, so that a change that
  re-pins a vanished analyzer while `master` holds findings nobody has accepted need not wait on a change that no
  scan could judge. Any other change of the scan's configuration answers for the whole tree by the base's
  declarations alone.
- **Narrowing a scan is a declaration too.** What a scan leaves out - an exclusion, the analyzer's options, the
  source scans' ignore and suppression lists, the lines of their scripts marked as choosing what their tools read -
  is said in a change of its own, which touches nothing else a scan reads (the script whose marked lines it changes
  is its own file), and the change after it is judged as it says ([Every change](#every-change)). A `.semgrepignore` and
  GitLab's custom ruleset (`.gitlab/sast-ruleset.toml`) are not said at all: `build/check-release-chain.py` refuses a
  tree that carries either, so that what *semgrep-sast* leaves out is said in one place, its `SAST_EXCLUDED_PATHS`.
- **Baselines are not declarations.** `build/ci/static-analysis-baseline.txt` and `build/ci/semgrep-baseline.txt` are
  closed lists of the findings that were in the tree before every change was gated: written when the scans began, on
  25 September 2026, and last added to in 6.3.6. A change may delete an entry whose finding is fixed, or end one that
  has no reason with its reason; it may never add a fingerprint or take a reason away, which *semgrep-sast-gate* holds
  on every change, and `build/ci/security-findings.py --write-baseline` keeps the entries still found and adds none.
  An entry with no reason is an open finding, not a dismissal: each is triaged - fixed, or given the reason it is not
  exploitable - by **31 March 2027**, and one of severity High before the next release. The eight High entries that
  had no reason were given theirs on 6 October 2026; then 475 of *static-analysis*'s 691 entries and 62 of
  *semgrep*'s 125 had none, as each file's header line counts.
- **Inline suppression comments are not declarations either.** `nosemgrep`, `NOLINT` and its forms, and
  `cppcheck-suppress` and its forms make a tool pass over a finding where they stand, with no reviewer and no expiry.
  No change adds one: *semgrep-sast-gate* holds a change that raises their count in any file a scan reads. Those
  already in the tree (44 in 23 files as the gate counted them on 6 October 2026) are triaged with the baselines, by
  the same dates.

### Before a release

The release job of a `v*` tag's pipeline (`build/ci/release-chain.py collect`) evaluates the security scans of
the tagged commit before it collects, let alone signs, anything, with `build/ci/security-gate.py`, and **refuses
the release** while:

- any of the four scans it needs did not finish: *static-analysis*, *semgrep*, *dependency-scan* (which must also
  have scanned the CI image's packages) and GitLab's *semgrep-sast*. Its report is missing, or says its status is
  not success. The pipeline of a tag without one of them is not created at all;
- a report is not the scan's of this pipeline: the tagged tree carries a scan's report as a tracked file, which
  would stand in for one the scan did not write, or *semgrep-sast*'s report was not written by GitLab's analyzer, or
  began before the tag's pipeline was created;
- *static-analysis* (cppcheck, clang-tidy) or *semgrep* found something that is not in its committed baseline;
- *semgrep-sast* found something of severity critical or high;
- *dependency-scan* found a vulnerability of severity critical or high that has a fixed version and that the VEX
  document does not dismiss as not affecting the project (one with no fixed version yet is read by hand, and one
  against a header-only package of the CI image, `linux-libc-dev`, is not affected and is said, never held:
  [Dependencies](#dependencies-software-composition-analysis));

unless `.gitlab/security-exceptions.yml` declares that finding not exploitable ([Declaring a finding not
exploitable](#declaring-a-finding-not-exploitable)) - `master`'s copy, which the job reads as it reads `master`'s
allow-list of tag keys, never the tagged tree's, so an exception that reaches `master` after the tag, through a merge
request of its own, counts when the job is retried. The file is the owner's (`.gitlab/CODEOWNERS`), is held to its
shape on every pipeline by `build/check-release-chain.py`, and says in its header how many findings it accepts, of
which scans, and how each group of them was read: the first 455, at 6.3.4, were each read on 26 September 2026 by two
AI review agents under the owner's delegation, as the file says, and for those the reviewer it names is the owner, who
delegated the reading. A dependency finding that does not affect the project at all goes in the VEX document instead.

This is the second evaluation, over the whole tree. The first is every change's ([Every change](#every-change)),
which answers for what a change adds; the release answers for the rest as well: a finding already on `master` in a
file no change has touched since, an advisory published after the change that brought its dependency, and
*dependency-scan*'s findings, which hold no change. A release made by hand runs the same scans on the project's Linux
machine against the release's commit and the same gate over their reports before anything is published.

### Secrets and credentials

- The repository holds no credential that opens anything: no token, no password, no signing key. The
  certificates and private keys under `hmailserver/test/SSL examples` are throwaway fixtures the regression suite
  uses to exercise TLS, trusted by nothing outside it.
- On GitHub, secret scanning with push protection refused a push that carried a real secret. On GitLab push
  protection is GitLab Ultimate, so the refusal happens before the push instead, on the pushing machine: the
  repository ships a pre-commit and a pre-push hook (`build/hooks/`; [CONTRIBUTING.md](CONTRIBUTING.md#secrets))
  that run `build/scan-secrets.py` - gitleaks from the image the `secret_detection` job of the clone's own working
  tree pins, with GitLab's rules and `.gitleaksignore` - and refuse the commit or the push on a finding, or when no
  scanner can run. git runs no
  hook a clone has not turned on, so each clone turns them on once (`git config core.hooksPath build/hooks`); a
  clone that has not is not refused by them. The maintainer's landing, the way changes reach `master`, runs the
  same scan over every commit a batch brings to `master` and pushes nothing when it finds one or cannot run. `.gitignore` leaves the usual key and credential files
  out of a `git add` (the test fixtures' directories apart). After the push, the `secret_detection` job scans the
  commits a push to `master` or a merge request brings (not a push to another branch without a merge request)
  and fails the pipeline on a finding that
  `.gitleaksignore` does not name. Both that job and `scan-secrets.py` run GitLab's rules but for the two entries
  of their global allowlist that left out every path under a directory named `build`, `bin`, `obj`, `target` or
  `packages`, this tree's `build/` among them: since 10 October 2026 both read those paths
  ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md#what-the-secret-scans-read)).
  gitleaks' inline allow marker on a secret's line waives nothing either: `scan-secrets.py` reports such a line,
  and the `checkers` job fails a tree that carries the marker, which the job's analyzer would take as a waiver.
  It has run in `master`'s pipelines since 27 September 2026: 2886389460 failed on
  twenty-four vetted test fixtures, which `.gitleaksignore` has named since, and 2892849251, on 29 September, passed. The
  whole history was scanned once, on 26 September 2026, with the job's own image on the project's Linux machine:
  thirty findings, each read and none a secret (test fixtures, the key parsers' test armour, AWS's documented
  example key), which `.gitleaksignore` names. The scans of a batch before its landing, the same day, found
  twelve more in the batch's commits - ten the first time, and two in folds made after it - all that same example
  key quoted in a document and the help pages made from it; the document describes it since, and the twelve are
  named too. The same scan on GitLab is a schedule the owner runs once
  ([hmailserver/docs/ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "Security scanning").
- The one credential the release automation needed on GitHub, for the Azure identity that Authenticode-signs
  the installer, was held in GitHub Actions' secrets. The identity itself is in Azure. On GitLab no secret is
  needed: the release job's ID token is exchanged through a federated credential made for one tag at a time
  ([RELEASE.md](RELEASE.md), "On GitLab, from 6.3.5"). The client secret made for the GitHub workflow is still
  on the app registration, valid until 10 September 2028. It is to be deleted.
  Release signatures are keyless (Sigstore, bound to the pipeline's own identity), so there is no signing key to
  protect. Release tags are signed with a maintainer's SSH key, whose public half is in `.gitlab/allowed_signers`.
- A secret is rotated when a maintainer leaves, when the pipeline that uses it changes hands, or on any suspicion
  of exposure. Every token that expires is replaced before its date
  ([What must stay valid](#what-must-stay-valid-for-a-security-release)).
- Test environments use throwaway credentials that the suite creates and destroys; none of them is a real
  account.

### Branch and tag names in the pipelines

The GitHub workflows that ran until 18 September 2026 were removed from the tree when the project moved to
GitLab; they are in the git history (unchanged from `faca023d4` to `d8ba4b646`, the last commit that has them). In them, `sign-release.yml`
took the tag through an environment variable and refused a tag that was lightweight, or not signed by a key in
the allowed signers file, before it signed anything, and `package-managers.yml` wrote the
tag into a command only after checking that it named a three-number version. `installer-smoke.yml` wrote its
`release_tag` input into a PowerShell command directly; only someone with write access to the repository could
dispatch it. Whatever replaces them on GitLab passes every branch and tag name through the environment, never
into the text of a command.
