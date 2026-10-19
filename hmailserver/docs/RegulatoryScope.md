Regulatory scope determination
==============================

**Status: out of scope as a manufacturer. Reviewed 12 August 2026.**

This is a dated, reviewable position on whether two pieces of EU legislation —
the Cyber Resilience Act and the revised Product Liability Directive — place
obligations on this project. It exists because both turn on *how the software is
supplied* rather than what it does, so the answer can change without a line of
code changing, and because the answer is much cheaper to write down now than to
reconstruct under pressure later.

It is a self-assessment by the maintainer, not legal advice. Anyone relying on it
commercially should take their own.

Who is asking
-------------

This fork is distributed as follows, and every conclusion below depends on these
facts staying true:

* AGPLv3, source public, no paid edition, no paid support, no hosted service.
* Released as an installer and source on GitLab
  (<https://gitlab.com/hmailserver/hmailserver>), which is canonical, and on
  GitHub, at <https://github.com/RainbowWizard-coder/hMailServer> under the
  account `RainbowWizard-coder` since 18 October 2026, where every release from
  6.0.0-B1 to 6.3.7 is a copy of GitLab's, with the same files byte for byte; a
  release is built, signed and published on GitLab first and then copied there.
  The project was on GitHub before, under the account
  `Progressiverobot`, until GitHub suspended that account on 18 September 2026;
  every release up to 6.3.3 whose files GitLab has is re-published there, byte
  for byte, with its original signatures (6.2.0 and 6.0.0-B4 are notes-only).
  No charge, no licence key, no mandatory registration, no telemetry. Where the
  project is hosted is not one of the review triggers below, and neither the
  move nor the return changed anything else on this list.
* Maintained by one individual, not a company or foundation.
* Donations, sponsorship and paid consulting: **none currently**, and see the
  triggers below before that changes.

Cyber Resilience Act — Regulation (EU) 2024/2847
------------------------------------------------

**In force** 10 December 2024. **Article 14 reporting obligations apply from 11
September 2026.** **Everything else — Annex I essential requirements, conformity
assessment, CE marking, technical documentation, declaration of conformity —
applies from 11 December 2027.**

Two independent questions decide whether it reaches this project.

**1. Is it made available in the course of a commercial activity?** That is the
test, and *free of charge is not the test*. Recital 18 says how development is
financed is not to be taken into account. The Commission's guidance under
Article 26 (document C(2026) 5252, approved 27 July 2026) confirms that freely
available open-source software is generally outside scope, and that **voluntary
donations, sponsorship, public funding, and paid consulting or training do not by
themselves make a project commercial**.

Commerciality would arrive with: selling the software; shipping a paid "pro"
edition; monetising a service built on it; requiring personal data beyond what
security or interoperability needs; or making donations effectively mandatory for
access or updates.

**2. Could it be an "open-source software steward" under Article 24?** No — a
steward must be a **legal person**. A sole individual maintainer is neither a
manufacturer (while non-commercial) nor eligible to be a steward.

**Conclusion: out of scope as manufacturer, ineligible as steward.**

**What changes it, and how much.** The moment there is a paid tier, a hosted
edition or a commercial licence exception, the whole product is in scope — and
because this fork substantially modifies upstream hMailServer, **this fork is the
manufacturer of record, not the upstream project.** Two consequences worth
knowing before that decision rather than after:

* A UK-based commercial manufacturer also needs an **EU authorised
  representative** (Article 18) — a real, recurring cost.
* Annex I would then require, in code as well as paper: secure-by-default
  configuration, no known exploitable vulnerabilities at release, security update
  distribution with integrity verification, an SBOM, coordinated vulnerability
  disclosure, and a defined support period.

Product Liability Directive — (EU) 2024/2853
--------------------------------------------

**Transposition deadline 9 December 2026**, applying to products placed on the
market from that date.

Software is now unambiguously a "product", including a standalone download. A
product can be **defective because of a cybersecurity vulnerability, or because
the manufacturer failed to supply security updates** — and liability under the
PLD **cannot be excluded or limited by contract.** The AGPLv3 warranty disclaimer
does not help here; that is the point of the provision.

The exemption is the same shape as the CRA's: **software supplied outside a
commercial activity.**

**Conclusion: out of scope, on the same basis, and it raises the stakes on the
CRA answer considerably.** Monetising would attach strict liability for security
defects across the EU from December 2026 — a full year *before* CRA conformity
obligations bite. That ordering is worth remembering: the liability arrives
first.

Things that do not reach this project
-------------------------------------

Recorded so the question does not get re-opened each time it comes up.

* **NIS2** — addresses operators of essential and important entities, not
  software vendors. It shapes what customers will *ask for* (logging, incident
  detection, message trace), which is a product argument, not a compliance one.
* **GDPR / UK GDPR** — this project is neither controller nor processor; the
  operator of a given installation is. That is precisely why per-account export,
  erasure reaching both the message store and the logs, and log retention limits
  are worth building: to let operators meet *their* obligations. They are
  features, not compliance for us.
* **DORA** — reaches financial entities and their critical ICT providers by
  contract. Not applicable to unpaid distribution.
* **eIDAS 2 / qualified electronic registered delivery** — a different service
  category entirely. Not relevant, and not planned.

Review triggers
---------------

Re-do this determination, with a new date, when **any** of the following happens:

1. Any paid offering appears — licence, support, hosting, a "pro" edition, or a
   commercial licence exception.
2. Donations or sponsorship become a condition of access or updates.
3. The maintainer incorporates, or maintenance moves to a company or foundation
   (which would make steward status possible, and change the analysis).
4. Distribution starts collecting personal data beyond security and
   interoperability purposes.
5. The Commission issues further Article 26 guidance, or harmonised standards
   under the CRA are published, that change the open-source treatment.

What we do anyway
-----------------

Not because either instrument compels it, but because they are the right shape
for a security-relevant product and they would be prerequisites if scope ever
changed:

* SPDX and CycloneDX SBOMs attached to every release from 6.2.3 (up to 6.3.3 by
  the GitHub workflow `sbom.yml`, in the git history at `v6.3.3`; on GitLab the
  `linux-sbom` job makes them), including the native
  dependencies — OpenSSL, Boost and libpq — that a source-tree scan does not see.
  The five releases before 6.2.3 that carry an installer have a CycloneDX SBOM of
  it alone, made afterwards from the installer itself
  ([SECURITY.md](../../SECURITY.md#supply-chain), *Supply chain*).
  That matters for exactly the purpose an SBOM has: without them, anyone checking a
  release against a CVE feed would have concluded this server does not link OpenSSL.
* **No executable binary is committed to the repository.** Forty were until 11
  September 2026. The ones the build still needs are fetched at build time from the
  project's `build-inputs-1` and `build-inputs-2` releases, or from the publisher,
  and checked against a SHA-256, gathered from
  the build machine's Visual Studio and checked by version, copied from Windows
  itself, or generated from this repository's own IDL, and every one is
  inventoried with an upstream, a version, a licence and the reason the build
  needs it ([`third-party-binaries.json`](third-party-binaries.json),
  explained in [ThirdPartyBinaries.md](ThirdPartyBinaries.md)). Until 18 September
  2026 every push and pull request ran
  the GitHub workflow `verify-binary-provenance.yml` (in the git history at
  `v6.3.3`), which failed if a binary appeared that the inventory did not describe, or one it
  lists as fetched was committed after all. The SBOM answers "what does this link
  against"; this answers the separate question "what unreviewable files are in
  the source tree, and who made them".
* **The release assets from 6.2.19 to 6.3.3 signed** - but for the two SBOMs of
  6.2.22-pre3, which carry no bundle - with Sigstore cosign, keyless via
  OIDC, the signature recorded in the public Rekor transparency log and verified in
  the same job so a malformed bundle cannot reach a user
  (the GitHub workflow `sign-release.yml`, in the git history at `v6.3.3`). Keyless
  deliberately: for a single-maintainer project a long-lived signing key held in CI
  is itself the thing most worth stealing.
* **The Windows installer Authenticode-signed since 6.3.1** (11 September 2026),
  with Azure Artifact Signing against a certificate profile issued to Progressive
  Robot Ltd, by the `authenticode` job of the same workflow
  ([RELEASE.md](../../RELEASE.md), *Authenticode*). Cosign and Authenticode are
  complementary rather than alternatives — SmartScreen and the UAC prompt read
  Authenticode and know nothing about Sigstore. The Linux packages carry no
  Authenticode, and the installers of the releases before 6.3.1 stay without it:
  the releases up to 6.3.3 are published with their original files, byte for
  byte, on GitLab and in its copy on GitHub - 6.2.0 and 6.0.0-B4 with none,
  which only the suspended account, or someone's old download, has
  ([Roadmap3.md](../../Roadmap3.md), *The files GitLab does not have*).
* A documented security policy and private disclosure route
  ([SECURITY.md](../../SECURITY.md)), plus a machine-readable `security.txt`
  (RFC 9116) served at `/.well-known/security.txt` by the web services listener.
  The same listener serves the other well-known paths this server answers:
  `/.well-known/acme-challenge/`, `/.well-known/mta-sts.txt`,
  `/.well-known/caldav` and `/.well-known/carddav`, and
  `/.well-known/openpgpkey/` - the OpenPGP Web Key Directory, which publishes
  public keys only and is off until an administrator, a domain and an account
  each say otherwise ([OpenPGP.md](OpenPGP.md)).
* Static analysis with CodeQL, and OpenSSF Scorecard, on every push to master on
  GitHub until 18 September 2026; neither runs on GitLab. Since 25 September 2026
  GitLab CI runs replacements for `master`, `batch*` branches and release tags,
  not yet blocking: cppcheck, clang-tidy and Semgrep, dependency scanning with
  Trivy and OSV, and a check of the repository settings Scorecard read
  ([ContinuousIntegration.md](ContinuousIntegration.md#security-scanning)).
  Since 18 October 2026 GitHub's own CodeQL default setup analyses the code of
  the project's repository on GitHub,
  <https://github.com/RainbowWizard-coder/hMailServer>, on each push to `master`,
  on each pull request and weekly, and the maintainers read its findings, nothing
  more: it is GitHub's, not a workflow of
  the project's. GitHub Actions runs one workflow of the project's there,
  `.github/workflows/scorecard.yml`, OpenSSF Scorecard over that copy.
  A full regression suite gating every release.
* Release notes that name unfixed known issues rather than omitting them.
* Security-update distribution with integrity verification, shipped in 6.2.28
  (8 September 2026): the server can check the release feed,
  fetch a newer installer with its Sigstore bundle, verify the bundle in-process
  against the release signers compiled into it, and apply it inside a
  configured window after a backup. A server up to 6.3.3 trusts one signer, the
  GitHub release workflow of the repository under the project's former account,
  `Progressiverobot`; a server built after 6.3.3 trusts that
  workflow for the releases up to 6.3.3 and this project's GitLab CI pipeline
  for the later ones, at the path the project began at and at the group's it
  moved to on 29 September 2026 (`SigstoreVerifier.cpp`,
  [UpdateFeed.md](UpdateFeed.md#what-a-server-built-after-633-checks)). The Roadmap's Product Liability Directive row
  was closed on 7 September 2026 against this document's determination.

What the release half of this list now depends on: up to 6.3.3 the SBOMs, the
Sigstore signatures, the Authenticode signature and the provenance check were all
made by GitHub Actions workflows, the Azure identity that signs the installer was
reached through GitHub Actions' secrets, and a server up to 6.3.3 accepts an
update only when its Sigstore bundle names that workflow's GitHub identity. None
of the workflows has run since GitHub suspended the project's account on 18
September 2026 but for two: `sbom.yml` and `sign-release.yml`, in the trees of
the old tags, ran once on 18 October 2026 in the repository the project has had on
GitHub again since that day, on the copies of the releases up to 6.2.22-pre6,
what they wrote was undone, and both have been disabled there since
([ASSURANCE-CASE.md](../../ASSURANCE-CASE.md)). The one workflow of the project's
that GitHub Actions runs there, OpenSSF Scorecard's, signs and publishes nothing
of a release: a release there is a copy of the one on GitLab, with the same files
byte for byte, and a release is built, signed and published on GitLab first and
then copied there. The releases up to 6.3.3 whose files GitLab has are on
GitLab with their original signatures (6.2.0 and 6.0.0-B4 are notes-only);
6.3.4 was built and Authenticode-signed by hand and carries no
Sigstore bundle or provenance; and the GitLab release jobs that replace the workflows made 6.3.5 on 29
September 2026, with its SBOMs, its Sigstore bundles and its Authenticode
signature, the Azure identity reached through a federated credential for the tag
and no secret ([ContinuousIntegration.md](ContinuousIntegration.md#the-release)). Moving the
chain off a single forge is its own row in [Roadmap2.md](../../Roadmap2.md),
*The release chain off a single forge*.

What in this document is checkable, and what is not
--------------------------------------------------

Worth separating, because the two halves have very different half-lives.

The **determination** — the scope analysis, the commerciality test, the steward
question, the PLD reasoning — is a dated legal self-assessment. Nothing in the
repository can confirm or refute it; it stands or falls on the legislation and on the
distribution facts listed under "Who is asking", and it is re-done when one of the
review triggers fires. Reviewed 12 August 2026.

The **"What we do anyway" list** is different: every item is a claim about this
repository, and each one is verifiable. Re-checked 13 August 2026 against
`.github/workflows/sbom.yml` (SPDX and CycloneDX, both attached on a `release`
event, with `build/merge-native-dependencies-into-sbom.ps1` adding OpenSSL, Boost
and libpq), `.github/workflows/sign-release.yml` (cosign `sign-blob` over every
asset, then `verify-blob` in the same job), `.github/workflows/codeql.yml`,
`.github/workflows/scorecard.yml`,
`.github/workflows/verify-binary-provenance.yml` (run against this tree: 40
manifest entries, 40 binaries found, 0 unlisted, every hash matching the bytes
stored in git), `.github/SECURITY.md`, and
`WebServicesServer`'s `/.well-known/security.txt` handler. (Those paths are as they
were that day: the workflows are in the git history at `v6.3.3`, and the policy
is `SECURITY.md` at the repository root.)

Re-checked 25 September 2026, for the move off GitHub: no tracked file is a binary
by the provenance workflow's own detector (its extension set, and its PE, ELF and
OLE sniff over `git ls-files`); every one of the manifest's 35 entries is marked as
not in git - 22 fetched, 10 gathered, 2 system, 1 generated - and none is tracked;
the `build-inputs-1` release is on GitLab with the three assets the manifest
hashes; `SECURITY.md` is at the repository root, and the `Policy:` line of the
served `security.txt` names it there on GitLab. The workflows named above are in
the tree, and none of them runs on GitLab.

This half is the one to re-read before quoting the page. A signing line in it has
been wrong twice for the same reason - a capability shipped and the paragraph
saying it had not was never revisited - the second time the Authenticode
paragraph, which called the signing outstanding for two weeks after 6.3.1 shipped
signed.
