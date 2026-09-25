# Security Policy

## Supported versions

| Version | Supported          |
| ------- | ------------------ |
| 6.3.x   | :white_check_mark: |
| < 6.3   | :x:                |

The 6.3 line is the one that receives security fixes. Its latest release is 6.3.3, published on 15 September
2026. A fix for a confirmed vulnerability ships in the next release; it is not back-ported to 6.2 or anything
older, because there are no maintenance branches. That is less of a burden than it sounds: an upgrade in place
keeps the configuration and the mail, and the database upgrade chain runs from every earlier hMailServer version,
so the answer to a vulnerability in an older version is to upgrade.

## Reporting a vulnerability

Report it privately, by either route:

- **A confidential issue on GitLab:**
  <https://gitlab.com/Progressiverobot/hmailserver/-/issues/new?issue[confidential]=true>. The link ticks
  *This issue is confidential* for you; check that it is still ticked before you submit, and choose the
  **Security** template. A confidential issue is visible only to you and the project's maintainers.
- **Email** to the project's Service Desk address:
  [contact-project+progressiverobot-hmailserver-86730559-issue-@incoming.gitlab.com](mailto:contact-project+progressiverobot-hmailserver-86730559-issue-@incoming.gitlab.com).
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

One known risk to the third target: the chain that signs and publishes a release ran on GitHub Actions, so while
GitHub is unavailable a fix can be written and tested but not released. Moving that chain off GitHub is
planned roadmap work and not yet done. If it holds a fix past 90 days, that is a slip like any other, and you
will be told before the target passes.

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

Releases are at <https://gitlab.com/Progressiverobot/hmailserver/-/releases>. Every release up to and including
6.3.3 was first published on GitHub. Where GitLab's releases page carries a file from one of them, it is the same
file, byte for byte, with the same signatures and Sigstore bundle, so everything below verifies it exactly as it
verified the original.

- **The release tag** (from `v6.2.23-alpha2` on) is an annotated tag signed with a maintainer's SSH key listed in
  `.github/allowed_signers`. That allow list is in the repository, so a clone can check it without trusting
  anything else:

  ```
  git -c gpg.ssh.allowedSignersFile=.github/allowed_signers verify-tag v6.2.23-alpha2
  ```

- **The assets of every release from 6.2.19 on** (the withdrawn 6.2.22-pre4 and the superseded -pre5 aside) carry
  Sigstore bundles (`<asset>.cosign.bundle`), keyless and recorded in the public Rekor log. For the releases up to
  6.3.3 the signing certificate names this repository's GitHub workflow, because that is where they were signed, so
  the identity and the issuer below are facts about those files and stay right for them wherever they are hosted.
  The bundle carries the certificate and the log's proof, and nothing in the check contacts GitHub. With
  [cosign](https://github.com/sigstore/cosign) installed:

  ```
  cosign verify-blob \
    --bundle hMailServer-x.y.z-x64.exe.cosign.bundle \
    --certificate-identity-regexp '^https://github\.com/Progressiverobot/hmailserver/' \
    --certificate-oidc-issuer https://token.actions.githubusercontent.com \
    hMailServer-x.y.z-x64.exe
  ```

  Only GitHub Actions can issue that identity, so while GitHub is unavailable no later release can carry it. The
  identity a later release does carry will be written here when the release chain moves.

- **Authenticode.** Since 6.3.1 the Windows installer is also Authenticode-signed; the Linux packages are not.
  Neither signature above replaces it: Windows SmartScreen and the UAC prompt read only Authenticode. In
  PowerShell, `Get-AuthenticodeSignature .\hMailServer-x.y.z-x64.exe` shows it.

## Supply chain

The third-party executables and libraries the build uses are not committed to this repository. They are
inventoried in [hmailserver/docs/ThirdPartyBinaries.md](hmailserver/docs/ThirdPartyBinaries.md): those fetched
from the project's `build-inputs-1` release are checked against a recorded SHA-256 whenever
`build/get-installer-binaries.ps1` places them, the MSVC runtime is checked by version, and the ADO type
libraries, which are Windows' own files, are checked only for presence, with their hash reported. The
script still downloads from the `build-inputs-1` release's GitHub address, which is unreachable; the release is
re-published on GitLab byte for byte, and [CONTRIBUTING.md](CONTRIBUTING.md#prerequisites) says how a new build
machine uses that copy.

OpenSSL, Boost and libpq are built from source by `libraries/build-*.ps1`, at the versions `hMailServer.vcxproj`
pins, and each script checks the SHA-256 of the source archive it downloads.

Releases from 6.2.3 on carry an SBOM, in SPDX and in CycloneDX (the withdrawn pre-release 6.2.22-pre4, and -pre5,
which -pre6 superseded, aside). They were made by `.github/workflows/sbom.yml`, which adds the native libraries to
the .NET dependency graph (`build/merge-native-dependencies-into-sbom.ps1`).

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
scanning"). They are not blocking yet: until their first findings are triaged, the thresholds below are held by
hand, and what runs is stated under each heading. A finding that is not fixed is suppressed only with a written
reason.

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
- **What ran on GitHub.** GitHub's dependency review (`.github/workflows/dependency-review.yml`) compared every
  pull request's dependency graph against the GitHub Advisory Database, including its malware advisories, as a
  required check: a change that added a dependency with a known vulnerability of severity high or critical, or a
  package flagged as malicious, could not be merged. Dependabot (`.github/dependabot.yml`) opened a pull request
  for every advisory touching a dependency in use, and for every new version of a GitHub Action the workflows pin.
  Neither runs on GitLab. *dependency-scan* stands for the alerts, not for the merge-time gate (it runs for the
  protected refs, and a merge request from a fork never reaches the runner it needs) nor for the update pull
  requests.
- **Remediation thresholds.** A high or critical advisory in a dependency is fixed, or the dependency replaced,
  **before the next release** and within 14 days of the advisory; moderate within 30 days; low within 90 days or
  with the next dependency refresh. A licence finding (a dependency whose licence is not compatible with
  AGPL-3.0-or-later) is treated as high.
- **Before a release.** A release is not cut while a known advisory of high or critical severity against a
  shipped dependency is open. The *dependency-scan* report of the release's commit is read first, and every
  component it marks UNKNOWN, with any the SBOM lists and the scan does not, is checked by hand against the
  components the release's SBOM lists (`hmailserver.spdx.json`, `hmailserver.cyclonedx.json`), which are also
  what a reader checks the shipped versions against.
- **Not affected.** A finding in a component that does not affect this project - the vulnerable code is not
  reached, the feature is not built, the platform is not one the server runs on - is recorded as a statement in
  the project's VEX document, [`.github/hmailserver.openvex.json`](.github/hmailserver.openvex.json) (OpenVEX),
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
- **What ran on GitHub.** CodeQL (`.github/workflows/codeql.yml`, configured in `.github/codeql/`) analysed the
  repository on every pull request and every push to `master`, and the `master` ruleset required its results. It
  does not run on GitLab: its command-line tool and its query packs are published only on GitHub.
- **Remediation thresholds.** An error-level finding, or a security finding of high or critical severity, is
  fixed before merge. A medium finding is fixed within 30 days; a low or note-level one with the next change to
  that file, or dismissed with a reason. A release is not cut with an open error-level finding.
- **Suppressions.** A finding is dismissed only with a reason that says why it is a false positive or not
  exploitable, recorded beside the finding.

### Secrets and credentials

- The repository holds no credential that opens anything: no token, no password, no signing key. The
  certificates and private keys under `hmailserver/test/SSL examples` are throwaway fixtures the regression suite
  uses to exercise TLS, trusted by nothing outside it.
- On GitHub, secret scanning with push protection refused a push that carried a real secret. Nothing scans pushes
  to GitLab yet.
- The one credential the release automation needed, for the Azure identity that Authenticode-signs the
  installer, was held in GitHub Actions' secrets; the identity itself is in Azure. Release signatures are keyless
  (Sigstore, bound to the workflow's own identity), so there is no signing key to protect. Release tags are signed
  with a maintainer's SSH key, whose public half is in `.github/allowed_signers`.
- A secret is rotated when a maintainer leaves, when the pipeline that uses it changes hands, or on any suspicion
  of exposure; the Azure identity's secret has an expiry and is replaced before it.
- Test environments use throwaway credentials that the suite creates and destroys; none of them is a real
  account.

### Branch and tag names in the pipelines

The GitHub workflows under `.github/workflows/` do not run on GitLab; they are kept in case GitHub returns. In
them, `sign-release.yml` takes the tag through an environment variable and refuses a tag that is lightweight, or
not signed by a key in `.github/allowed_signers`, before it signs anything, and `package-managers.yml` writes the
tag into a command only after checking that it names a three-number version. `installer-smoke.yml` writes its
`release_tag` input into a PowerShell command directly; only someone with write access to the repository can
dispatch it. Whatever replaces them on GitLab passes every branch and tag name through the environment, never
into the text of a command.
