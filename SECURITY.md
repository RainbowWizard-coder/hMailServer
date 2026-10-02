# Security Policy

## Supported versions

| Version | Supported          |
| ------- | ------------------ |
| 6.3.x   | :white_check_mark: security fixes until 30 September 2027 ([While releases pause](#while-releases-pause)) |
| < 6.3   | :x:                |

The 6.3 line is the one that receives security fixes. Its latest release is 6.3.6, published on 1 October
2026. A fix for a confirmed vulnerability ships in the next release; it is not back-ported to 6.2 or anything
older, because there are no maintenance branches. That is less of a burden than it sounds: an upgrade in place
keeps the configuration and the mail, and the database upgrade chain runs from every earlier hMailServer version,
so the answer to a vulnerability in an older version is to upgrade.

From 6.3.6, releases pause: no feature release follows it until the maintainers say otherwise here. Security
releases still do, as [While releases pause](#while-releases-pause) says, below.

## Reporting a vulnerability

Report it privately, by either route:

- **A confidential issue on GitLab:**
  <https://gitlab.com/hmailserver/hmailserver/-/issues/new?issue[confidential]=true>. The link ticks
  *This issue is confidential* for you; check that it is still ticked before you submit, and choose the
  **Security** template. A confidential issue is visible only to you and the project's maintainers.
- **Email** to the project's Service Desk address:
  [contact-project+hmailserver-hmailserver-86730559-issue-@incoming.gitlab.com](mailto:contact-project+hmailserver-hmailserver-86730559-issue-@incoming.gitlab.com).
  GitLab files the message as a confidential issue, and the maintainers' replies reach you by email, so no
  GitLab account is needed.

Do **not** open a public issue or merge request, or edit the wiki, for a security problem, and do not post it to
the hMailServer forum. A public report starts the clock for everyone running the server, not just for us. If a
report was made public by mistake, say so by either route above and the maintainer makes it confidential.

This file used to send reports to GitHub Security Advisories. GitHub has been unavailable to the project since
18 September 2026, so if you reported something there and had no answer, send it again by one of the routes
above.

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

6.3.6 is the last release for the time being, the owner's decision of 28 September 2026. What follows is **the
maintainers' decision of 29 September 2026**, taken under the owner's delegation. The maintainers write here when
it changes.

### What is supported, and for how long

- **The 6.3 line, as its newest release:** 6.3.6, and each security release made after it (6.3.7, 6.3.8 and so
  on). A security release is 6.3.6 with fixes. During the pause `master` takes fixes only, and a security release
  is cut from `master` like every other release. There is still no maintenance branch, and nothing is
  back-ported. An installation of 6.3.5, 6.2 or anything older is upgraded in place to the newest release, as
  [Supported versions](#supported-versions) says.
- **Until 30 September 2027.** Before that date, the maintainers write here whether support continues. If nothing
  has been written, the 6.3 line is not supported from 1 October 2027, and the table above is changed to say so.
- **Why that date.** It is as far as the project's own record reaches:
  - the last of the findings that `.gitlab/security-exceptions.yml` accepts expire on 25 and 27 September 2027.
    After its expiry, a finding holds every release again until it has been read again;
  - the GitLab tokens used by the maintainers' machine, Renovate and SonarQube Cloud expire on 26 and
    27 September 2027 ([What must stay valid](#what-must-stay-valid-for-a-security-release), below).

  A later date would be a promise that rests on renewals nobody has made yet. A year is also long enough for
  several reports to run through the 90-day timetable one after another.
- **One date comes first.** 298 of the accepted findings (263 of *static-analysis*, 31 of *semgrep-sast* and 4 of
  *semgrep*) expire on 26 and 27 March 2027. From the day after, the release job refuses a release while any of
  them is still found and has not been read again (`build/ci/security-gate.py`). So the maintainers read them again
  before 26 March 2027, and give each a new expiry. Otherwise, a
  report that arrives after that date waits on that reading as well as on its fix.

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

### How a report is handled

The routes, the targets and the 90-day disclosure timetable above are unchanged. Security response rests on
Christopher Holloway ([GOVERNANCE.md](GOVERNANCE.md#maintainers)). Zain Ul Abidin can see every report, and
taking the duty on is the maintainers' decision.

1. **Acknowledge** within 5 working days, and **assess** within 10: reproduce it, or ask for what is missing.
2. **Fix** with a regression test that fails against the build before the fix. The fix is built and tested by
   hand on the maintainers' own machines before anything is pushed: the Windows suite on a bench and the Linux
   suite on linix ([RELEASE.md](RELEASE.md), "A security release during the pause").
3. **Push only when that has passed.** The project's pipelines run only for what is on GitLab, and its
   repository is public. So from the push of the fix until the release is published, the fix can be seen and the
   advisory cannot. That is about a day: the batch's pipeline, then the tag's. Keep that window short. The
   commit message and the version's `CHANGELOG.md` section, which are public from the push, say what was fixed
   and credit the reporter. They do not say how to exploit it.
4. **Release** it ([How a security release is made](#how-a-security-release-is-made), below).
5. **Disclose** once the release is on the update feed: make the issue public, request the CVE ID from it if that
   has not been done, and announce the release.

### How a security release is made

A security release is a release like any other, made by the tag's pipeline ([RELEASE.md](RELEASE.md), "A security
release during the pause", has the steps). Five of the steps are the owner's, and no other person can take them.
The project access token `laptop-git` holds the Owner role as well ([What must stay
valid](#what-must-stay-valid-for-a-security-release), below), so GitLab would let steps 1 and 4 be taken with it:

1. **Landing the fix.** Zain Ul Abidin approves the batch's merge request and the owner lands it
   (`build/land-via-mr.py`). When waiting would take a fix past its disclosure window, the owner may land it
   without that approval, giving the reason (`--land-without-approval`, [GOVERNANCE.md](GOVERNANCE.md), Review).
   The change is then reviewed afterwards.
2. **The Azure federated credential for that one tag**, made before the tag is pushed. Its subject names the
   project's path at that moment, `hmailserver/hmailserver` since the project moved into the group on 29 September
   2026 ([RELEASE.md](RELEASE.md), "On GitLab, from 6.3.5"). Without it, `release-authenticode` cannot sign the
   installer, and no later job runs.
3. **The release tag, signed with the one key in `.gitlab/allowed_signers`.** That is the owner's key, on his own
   machine. `release` refuses a tag signed by any other key.
4. **Starting `release`, and later `release-publish`, as the owner.** He is the only person among the project's
   members with the Maintainer role or above; the one other member with such a role is the project access token
   `laptop-git`, an Owner. The package protection rules let only an Owner upload a release's files.
5. **Removing the federated credential** once the release is published.

Two more things are needed besides:

- the project's two runners, linix and bench150, must be up
  ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "The runners");
- the installer smoke test is run by hand between `release-sign` and `release-publish`.

If the Azure side cannot sign, the pipeline cannot publish a release. This could happen if the identity
validation has lapsed and cannot be renewed in time. The fallback is [a release made by
hand](RELEASE.md#a-release-made-by-hand), as 6.3.4 was: its sums are signed with the tag's key, and it carries no
Sigstore bundle. No server installs such a release by live update. Each installation is upgraded by hand.

### What must stay valid for a security release

What a release depends on that expires, as recorded on 29 September 2026. The GitLab tokens' dates were read
through GitLab's API that day. No value of any of them is in the repository, and none is written here.

| What | Used for | Expires |
|---|---|---|
| The release-tag signing key, the one key in `.gitlab/allowed_signers` | Signing each release tag, on the owner's machine | An SSH key has no expiry. A lost key is replaced by putting its successor on `master` ([GOVERNANCE.md](GOVERNANCE.md#critical-assets)) |
| The Azure federated credential | `release-authenticode`'s exchange of the job's ID token | It does not expire. It is made for one tag and removed after that release. An app registration holds twenty at most |
| Azure Artifact Signing's identity validation | Every Authenticode signature | It must be renewed, and signing stops if it lapses. **The date is not recorded here**: it is read in the Azure portal (the Artifact Signing account, *Identity validations*), which has no API for it |
| The Azure subscription (pay-as-you-go) | The Artifact Signing account | It does not expire, but it must be kept paid |
| The app registration's client secret, made for the GitHub workflow on 10 September 2026 | Nothing since the move to GitLab: the pipeline needs no secret | 10 September 2028. It should be deleted (`az ad app credential delete`), not left to run out |
| `.gitlab/security-exceptions.yml`, 469 accepted findings | The release job's security gate | Each finding has its own date: 286 expire on 26 March 2027, 12 on 27 March 2027, 169 on 25 September 2027 and 2 on 27 September 2027. After its date, a finding holds every release until it has been read again and given a new expiry |
| `RENOVATE_TOKEN`, the owner's personal access token `renovate` | Renovate's merge requests | 27 September 2027 |
| `sonarcloud-hmailserver`, the owner's personal access token | SonarQube Cloud's binding to the GitLab group | 27 September 2027 |
| `laptop-git`, a project access token with the Owner role and the `api` scope | Work from the maintainers' machine. No CI job uses it: the jobs use their own `CI_JOB_TOKEN`. As an Owner it can do in the project what the owner's account can, landing a change and starting the release jobs included | 27 September 2027 |
| `glab-laptop`, the owner's personal access token | `glab` on the maintainers' machine, which `build/land-via-mr.py` needs | 26 September 2027 |
| `GITLAB_AUTH_TOKEN`, the owner's personal access token for Scorecard | The weekly Scorecard job | Not made yet (the variable does not exist). A year from when it is made ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "OpenSSF Scorecard") |
| `HM_EXPORT_TOKEN`, and `HM_EXPORT_FULL_TOKEN` if set | The weekly export ([Mirroring.md](hmailserver/docs/Mirroring.md)) | Not made yet. The date is chosen when a token is made. The job names it from 30 days before |
| `HM_REPO_SIGNING_KEY`, the apt and dnf repositories' signing subkey | `package-repos` | Not made yet. Five years from when it is made. The job warns from 180 days before ([PackageManagers.md](hmailserver/docs/PackageManagers.md)) |
| The release channels' credentials (`PSGALLERY_API_KEY`, `SNAPCRAFT_STORE_CREDENTIALS`, `DOCKERHUB_TOKEN`, `QUAY_ROBOT_TOKEN`, `AUR_SSH_KEY`, `OBS_RUNSERVICE_TOKEN`, `LAUNCHPAD_SIGNING_KEY`, `COPR_WEBHOOK_URL`) | Publishing a release to its channels, not making it ([Channels.md](hmailserver/docs/Channels.md)) | None made yet. The PowerShell Gallery's key lasts 365 days at most, and the snap's credentials carry a date. The Copr webhook does not expire. The rest expire as set when made |
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

Releases are at <https://gitlab.com/hmailserver/hmailserver/-/releases>. Every release up to and including
6.3.3 was first published on GitHub. Where GitLab's releases page carries a file from one of them, it is the same
file, byte for byte, with the same signatures and Sigstore bundle, so everything below verifies it exactly as it
verified the original.

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

- **The assets of every release from 6.2.19 on** (the withdrawn 6.2.22-pre4 and the superseded -pre5 aside) carry
  Sigstore bundles (`<asset>.cosign.bundle`), keyless and recorded in the public Rekor log. For the releases up to
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
  release's SLSA v1 build provenance: an in-toto statement that names every file of the release by its SHA-256 - the
  installers, every package, the AppImages, the source of the libraries linked into their runtime and of the libraries
  they carry, the source tarball, the chart, the sums and every SBOM - with the commit it
  was built from and the tag's pipeline as the builder, signed by that pipeline with Sigstore (a bundle holding a DSSE
  envelope, recorded in the public Rekor log). cosign is the verifier; the SLSA project's `slsa-verifier` is not
  needed. To check that a download is one the tag's pipeline built:

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
  The file is named for what it is, a Sigstore bundle (`.sigstore.json`, the name Sigstore's clients take), and not
  `.intoto.jsonl`, the SLSA generator's name for JSON Lines of bare DSSE envelopes, which the GitHub releases up to
  6.3.3 carried: OpenSSF Scorecard, which reads a file's name alone, counts it as a signature rather than as
  provenance, and a copy under the other name would misdescribe its format to get the higher score.

- **Authenticode.** Since 6.3.1 the Windows installer is also Authenticode-signed, and from 6.3.6 the PowerShell
  module's `HmailServer.psd1` and `HmailServer.psm1` in the release; the Linux packages are not.
  Neither signature above replaces it: Windows SmartScreen and the UAC prompt read only Authenticode, and
  PowerShell's AllSigned policy reads it for a module. In PowerShell,
  `Get-AuthenticodeSignature .\hMailServer-x.y.z-x64.exe` shows it.

## Supply chain

The third-party executables and libraries the build uses are not committed to this repository. They are
inventoried in [hmailserver/docs/ThirdPartyBinaries.md](hmailserver/docs/ThirdPartyBinaries.md): those fetched
from the project's `build-inputs-1` release are checked against a recorded SHA-256 whenever
`build/get-installer-binaries.ps1` places them, the MSVC runtime is checked by version, and the ADO type
libraries, which are Windows' own files, are checked only for presence, with their hash reported. The
script downloads the `build-inputs-1` release's files from the project's package registry on GitLab, where they
were re-published byte for byte after GitHub suspended the project's account
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

## Vulnerability management policy

What is checked, on what, and what happens when it finds something. On GitHub these checks were automatic and
blocking. GitHub has been unavailable since 18 September 2026. Since 25 September 2026 `.gitlab-ci.yml` has
replacements for the source and dependency scans and for OpenSSF Scorecard, which run on the project's own
runner for `master`, `batch*` branches and release tags
([hmailserver/docs/ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md#security-scanning), "Security
scanning"). They do not stop a push's pipeline yet: until their first findings are triaged, the thresholds below are
held by hand for a change, and what runs is stated under each heading. **A release is held by them** since 26
September 2026: the release job reads the reports of four of them and refuses while one did not finish or found
something the policy counts ([Before a release](#before-a-release)). A finding that is not fixed is suppressed only
with a written reason.

### Dependencies (software composition analysis)

- **What runs now.** The *dependency-scan* job of `.gitlab-ci.yml` runs Trivy over the twelve NuGet lock files,
  the three `packages.config` files and the scanners' own hashed Python requirements, and over the package
  database of the image the Linux server is built in, whose OpenSSL, libpq, libldap and Boost the Linux packages
  are built against. It asks OSV (`api.osv.dev`) about the libraries the Windows server is built with and no
  manifest names - OpenSSL, Boost, libpq, zlib, QuickJS, MariaDB Connector/C, 7-Zip - at the versions the build
  uses, and says UNKNOWN, not clean, for a library OSV holds no usable data on (libpq, QuickJS, 7-Zip 19.00 and
  SQL Server Compact, today). It honours the VEX document below. Renovate (`renovate.json`, the scheduled `renovate` job) is built to open
  a merge request at once for every OSV advisory against a dependency in use, and a weekly one per ecosystem; it
  waits for the owner's token and schedule ([hmailserver/docs/ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md),
  "Dependency updates"). What holds besides: the NuGet lock files are
  committed and the .NET projects are restored with `--locked-mode`, so a package cannot change under a build
  without a reviewed commit; the native libraries are pinned by version and by source hash, as above; and every
  third-party binary is checked against `hmailserver/docs/third-party-binaries.json`.
- **What ran on GitHub.** GitHub's dependency review (the workflow `dependency-review.yml`, in the git history at
  `v6.3.3`) compared every
  pull request's dependency graph against the GitHub Advisory Database, including its malware advisories, as a
  required check: a change that added a dependency with a known vulnerability of severity high or critical, or a
  package flagged as malicious, could not be merged. Dependabot (`dependabot.yml`, likewise) opened a pull request
  for every advisory touching a dependency in use, and for every new version of a GitHub Action the workflows pin.
  Neither runs on GitLab. *dependency-scan* stands for the alerts, not for the merge-time gate (it runs for the
  protected refs, and a merge request from a fork never reaches the runner it needs) nor for the update pull
  requests.
- **Remediation thresholds.** A high or critical advisory in a dependency is fixed, or the dependency replaced,
  **before the next release** and within 14 days of the advisory; moderate within 30 days; low within 90 days or
  with the next dependency refresh. A licence finding (a dependency whose licence is not compatible with
  AGPL-3.0-or-later) is treated as high.
- **Before a release.** A release is not cut while a known advisory of high or critical severity against a
  shipped dependency is open. The release job refuses one that has a fixed version
  ([Before a release](#before-a-release)); one with no fixed version yet is not refused by the job, and is read
  from the report by hand before the release: the dependency replaced, the advisory dismissed in the VEX document
  with its reason, or the release waits. The *dependency-scan* report of the release's commit is read first, and every
  component it marks UNKNOWN, with any the SBOM lists and the scan does not, is checked by hand against the
  components the release's SBOM lists (`hmailserver.spdx.json`, `hmailserver.cyclonedx.json`), which are also
  what a reader checks the shipped versions against.
- **Not affected.** A finding in a component that does not affect this project - the vulnerable code is not
  reached, the feature is not built, the platform is not one the server runs on - is recorded as a statement in
  the project's VEX document, [`.gitlab/hmailserver.openvex.json`](.gitlab/hmailserver.openvex.json) (OpenVEX),
  with the reason, when it is dismissed. An empty statement list means every advisory raised against a component
  has been fixed rather than dismissed. *dependency-scan* reads the document, so a statement there is what quiets
  a finding the scan keeps raising.

### Source code (static analysis)

- **What runs now.** The C++ server is compiled with the toolchain's warnings as errors (`/WX`), the regression
  suite runs the server under its crash oracle before every release, first on a build that keeps every internal
  assertion (RELEASE.md step 8b), and the fuzz harnesses under `fuzz/` run for a set time on the release source
  of every minor release (step 8c). `build\analyze.ps1` runs MSVC's `/analyze` by hand. In `.gitlab-ci.yml`,
  *static-analysis* runs cppcheck and clang-tidy (the Clang static analyser among its checks) over every
  translation unit the Linux build compiles, and *semgrep* runs Semgrep's open rules over the C#, Python,
  JavaScript and shell sources, the Control Deck's and the webmail's scripts among them. Each holds its findings
  to a committed baseline (`build/ci/static-analysis-baseline.txt`, `build/ci/semgrep-baseline.txt`), so that it
  fails on a new finding only, and fails the pipeline then (since 6.3.6; before, both were allow_failure while
  their baselines were triaged). Neither reads the
  Windows-only sources (the COM API, the service host, the ADO and SQL Server Compact backends), which only an
  MSVC build compiles.
- **What ran on GitHub.** CodeQL (the workflow `codeql.yml` and its configuration, in the git history at `v6.3.3`) analysed the
  repository on every pull request and every push to `master`, and the `master` ruleset required its results. It
  does not run on GitLab: its command-line tool and its query packs are published only on GitHub.
- **Remediation thresholds.** An error-level finding, or a security finding of high or critical severity, is
  fixed before merge. A medium finding is fixed within 30 days; a low or note-level one with the next change to
  that file, or dismissed with a reason. A release is not cut with an open error-level finding.
- **Suppressions.** A finding is dismissed only with a reason that says why it is a false positive or not
  exploitable, recorded beside the finding: in `.gitlab/security-exceptions.yml`, with an expiry, for a finding
  that would hold a release ([Before a release](#before-a-release)).

### Before a release

The release job of a `v*` tag's pipeline (`build/ci/release-chain.py collect`) evaluates the security scans of
the tagged commit before it collects, let alone signs, anything, with `build/ci/security-gate.py`, and **refuses
the release** while:

- any of the four scans it needs did not finish: *static-analysis*, *semgrep*, *dependency-scan* (which must also
  have scanned the CI image's packages) and GitLab's *semgrep-sast*. Its report is missing, or says its status is
  not success. The pipeline of a tag without one of them is not created at all;
- *static-analysis* (cppcheck, clang-tidy) or *semgrep* found something that is not in its committed baseline;
- *semgrep-sast* found something of severity critical or high;
- *dependency-scan* found a vulnerability of severity critical or high that has a fixed version and that the VEX
  document does not dismiss as not affecting the project (one with no fixed version yet is read by hand:
  [Dependencies](#dependencies-software-composition-analysis), "Before a release");

unless `.gitlab/security-exceptions.yml` accepts that finding - `master`'s copy, which the job reads as it reads
`master`'s allow-list of tag keys, never the tagged tree's. An entry there names the scan and the finding (the
fingerprint the gate prints; for a dependency the advisory's id and the package), what it is, why it is accepted -
not exploitable as the code stands, or waiting on an upstream fix - the GitLab user of the maintainer who read it,
the day, and an expiry no more than a year later, after which the finding holds the release again. The file is
the owner's (`.gitlab/CODEOWNERS`), is held to that shape on every pipeline by `build/check-release-chain.py`,
and at 6.3.4 accepts 455 findings judged not exploitable - 188 of GitLab's *semgrep-sast*, 263 of
*static-analysis* and 4 of *semgrep* - each read on 26 September 2026 by two AI review agents under the
owner's delegation, as the file says; for those, the reviewer it names is the owner, who
delegated the reading. A dependency finding that does not affect the project at all goes in the VEX document
instead.

This holds releases, not changes: the scans run after a push, for the protected refs, and a finding a change
brings stops the next release until it is fixed or accepted. A release made by hand runs the same
scans on the project's Linux machine against the release's commit and the same gate over their reports before
anything is published.

### Secrets and credentials

- The repository holds no credential that opens anything: no token, no password, no signing key. The
  certificates and private keys under `hmailserver/test/SSL examples` are throwaway fixtures the regression suite
  uses to exercise TLS, trusted by nothing outside it.
- On GitHub, secret scanning with push protection refused a push that carried a real secret. On GitLab push
  protection is GitLab Ultimate, so the refusal happens before the push instead, on the pushing machine: the
  repository ships a pre-commit and a pre-push hook (`build/hooks/`; [CONTRIBUTING.md](CONTRIBUTING.md#secrets))
  that run `build/scan-secrets.py` - gitleaks from the image the `secret_detection` job pins, with GitLab's rules
  and `.gitleaksignore` - and refuse the commit or the push on a finding, or when no scanner can run. git runs no
  hook a clone has not turned on, so each clone turns them on once (`git config core.hooksPath build/hooks`); a
  clone that has not is not refused by them. The maintainer's landing, the way changes reach `master`, runs the
  same scan over every commit a batch brings to `master` and pushes nothing when it finds one or cannot run. `.gitignore` leaves the usual key and credential files
  out of a `git add` (the test fixtures' directories apart). After the push, the `secret_detection` job scans the
  commits a push to `master` or a merge request brings (not a push to another branch without a merge request)
  and fails the pipeline on a finding that
  `.gitleaksignore` does not name. It has run in `master`'s pipelines since 27 September 2026: 2886389460 failed on
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
