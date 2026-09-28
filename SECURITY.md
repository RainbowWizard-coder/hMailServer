# Security Policy

## Supported versions

| Version | Supported          |
| ------- | ------------------ |
| 6.3.x   | :white_check_mark: |
| < 6.3   | :x:                |

The 6.3 line is the one that receives security fixes. Its latest release is 6.3.5, published on 28 September
2026. A fix for a confirmed vulnerability ships in the next release; it is not back-ported to 6.2 or anything
older, because there are no maintenance branches. That is less of a burden than it sounds: an upgrade in place
keeps the configuration and the mail, and the database upgrade chain runs from every earlier hMailServer version,
so the answer to a vulnerability in an older version is to upgrade.

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
release"), is written and has not run yet; until it has, a release is built and signed by hand, as 6.3.4 was. If
that holds a fix past 90 days, that is a slip like any other, and you will be told before the target passes.

## Coordinated disclosure

We follow coordinated disclosure on a **90 day** timetable. The vulnerability is published when a fix ships, or
at 90 days, whichever comes first. If you intend to disclose sooner than that, say so in the report so we can
plan around it rather than discover it.

You keep credit for the finding unless you ask us not to name you. We will not ask you to sign anything, and we
do not pay bounties.

**CVE IDs.** GitLab is a CVE Numbering Authority for public projects hosted on GitLab.com. A CVE ID is requested
from the vulnerability's confidential issue, through GitLab's *Create CVE ID Request*, and quoted in the release
notes.

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
  with the issuer `https://gitlab.com`. 6.3.5, published on 29 September 2026, was the first, and carries the
  identity `https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v6.3.5`, the path the project
  began at, with the project's id, `86730559`, in the certificate. The project moved into the group
  [`hmailserver`](https://gitlab.com/hmailserver) later that day, and the releases after 6.3.5 carry the identity
  `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`, with the same issuer and id. A
  server from 6.3.4 on trusts both paths, the group's only with that id, whatever its update settings say
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).

- **Authenticode.** Since 6.3.1 the Windows installer is also Authenticode-signed; the Linux packages are not.
  Neither signature above replaces it: Windows SmartScreen and the UAC prompt read only Authenticode. In
  PowerShell, `Get-AuthenticodeSignature .\hMailServer-x.y.z-x64.exe` shows it.

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
| `hmailserver_<version>_amd64.deb`, `hmailserver-<version>-1.x86_64.rpm` | `<package>.cdx.json` | The libraries the Linux server needs from the distribution (glibc, libstdc++, OpenSSL, zlib, libpq, OpenLDAP), each with the development package and version it was built against; Boost, linked into the binary, at its exact version; the QuickJS compiled into it; and what the package itself declares it depends on |
| the container image, once one is published | an attestation on the image (predicate type `cyclonedx`) | Every distribution package in the image, as Syft reads it, and what the `.deb`'s own SBOM names inside the binary |
| the source tree | `hmailserver.spdx.json`, `hmailserver.cyclonedx.json` | Syft's inventory of the tree, with the native libraries it cannot see added |

Each document's subject (`metadata.component`) is its file, by SHA-256 and SHA-512, so it cannot be mistaken for
another file's. `build/asset-sbom.py` makes them: the release jobs of `.gitlab-ci.yml` make the packages' in
`linux-build`, where the build ran, and the installer's in `release-sbom`, from the bytes Authenticode signed, and
refuse to publish a release whose compiled file has no SBOM or one that describes other bytes. The apt and dnf
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
"The container image"), the SBOM is an attestation stored beside the image in the registry, signed by the same
pipeline identity as its signature:

```
cosign verify-attestation --type cyclonedx \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  registry.gitlab.com/hmailserver/hmailserver@sha256:<digest> | jq -r .payload | base64 -d | jq .predicate
```

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

- **What runs now.** The *dependency-scan* job of `.gitlab-ci.yml` runs Trivy over the eleven NuGet lock files,
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
  can fail on a new finding only; both are allow_failure until the baselines are triaged. Neither reads the
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
  `.gitleaksignore` does not name. It has not run on GitLab yet: `master` is still pushed with `ci.skip`. The
  whole history was scanned once, on 26 September 2026, with the job's own image on the project's Linux machine:
  thirty findings, each read and none a secret (test fixtures, the key parsers' test armour, AWS's documented
  example key), which `.gitleaksignore` names. The scans of a batch before its landing, the same day, found
  twelve more in the batch's commits - ten the first time, and two in folds made after it - all that same example
  key quoted in a document and the help pages made from it; the document describes it since, and the twelve are
  named too. The same scan on GitLab is a schedule the owner runs once
  ([hmailserver/docs/ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "Security scanning").
- The one credential the release automation needed, for the Azure identity that Authenticode-signs the
  installer, was held in GitHub Actions' secrets; the identity itself is in Azure. Release signatures are keyless
  (Sigstore, bound to the workflow's own identity), so there is no signing key to protect. Release tags are signed
  with a maintainer's SSH key, whose public half is in `.gitlab/allowed_signers`.
- A secret is rotated when a maintainer leaves, when the pipeline that uses it changes hands, or on any suspicion
  of exposure; the Azure identity's secret has an expiry and is replaced before it.
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
