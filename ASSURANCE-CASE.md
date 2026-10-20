Assurance case
==============

This document states what hMailServer claims about its own security, and the
argument and evidence for each claim. It is written for someone deciding
whether to run this on the public internet, and for anyone auditing that
decision.

It is deliberately *not* a marketing document. Where a claim is limited, the
limit is stated. Where a risk is accepted rather than eliminated, it is named
in [Residual risk](#8-residual-risk).

**Status since 18 September 2026.** The project's GitHub account,
`Progressiverobot`, was suspended on 18 September 2026 and stays so, and the
project's home has been
[GitLab](https://gitlab.com/hmailserver/hmailserver) since 22 September
2026 (in the group `hmailserver` since 29 September 2026). Since 18 October
2026 the project is published on GitHub again, at
<https://github.com/RainbowWizard-coder/hMailServer>, as a second home, and
GitLab is canonical: the project's pipelines run there and nowhere else, and a
release is built, signed and published there first.
Claim C7 rests on the release chain that ran on GitHub Actions
(`sign-release.yml`, `sbom.yml`, `verify-binary-provenance.yml`, in the git
history at `v6.3.3`). It holds for the releases that chain signed, 6.2.19 to
6.3.3, whose Sigstore bundles verify without GitHub. 6.3.4 was built and signed
by hand and carries no Sigstore bundle. The chain's replacement, the release jobs of `.gitlab-ci.yml`
([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md#the-release)),
made 6.3.5 on 29 September 2026: every file of it carries a Sigstore bundle
signed by the tag's pipeline, which verifies without GitHub, so the claim holds
for 6.3.5 and for each later release the jobs make. Since 18 October 2026 the
releases are published on GitHub too - every one from 6.0.0-B1 to 6.3.7 that
day, and each later one after GitLab has published it - with the same files
byte for byte, each checked against the SHA-256 that GitLab's package registry
records for it, under the same names but for the per-distribution `.deb`
files, whose `~` GitHub writes as `.`. GitHub's immutable-releases setting is on
there, so a release published after the copies of 18 October 2026 cannot be
changed; the copies themselves, published before the setting was switched on,
can.
Likewise the CodeQL, Dependabot and dependency-review checks cited below ran on
GitHub until 18 September 2026 and do not run on GitLab. No pipeline of the
project's runs on GitHub. Two workflows of the project's are in the tree, for
the GitHub copy alone: `.github/workflows/scorecard.yml`, which scores it with
OpenSSF Scorecard and publishes the result to scorecard.dev and to that
repository's code scanning, the job's two write permissions being for those two
publications alone; and `.github/workflows/codeql.yml`, which analyses its code
with CodeQL on each push to `master`, on each pull request and weekly, and whose
one write permission is the upload of its results to code scanning. It takes the
place of GitHub's CodeQL default setup, which did the same from 18 October 2026
with no workflow file, and which Scorecard's SAST check therefore did not count
([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md#openssf-scorecard)).
Neither builds, signs or publishes anything of a release. GitHub Actions is
enabled there for those two and for the jobs GitHub runs itself to build the
repository's dependency graph; it allows GitHub's own actions and
`ossf/scorecard-action` alone, each pinned by a full commit. The maintainers read
CodeQL's findings, nothing more.
Old workflows ran there once. The tags from `v6.2.3` to `v6.3.3`, and
`build-inputs-1`, still carry the chain's `sbom.yml` in their trees, and those
from `v6.2.19`, and `build-inputs-1`, its `sign-release.yml`, and GitHub ran them
on the publication of
the copies of the releases from 6.2.3 to 6.2.22-pre6 on 18 October 2026
(`sign-release.yml` failed for `v6.2.19`): they wrote SBOMs and signature bundles
of their own over 62 of the copied files and added three. Each of the 62 was put
back from the copy checked against GitLab's SHA-256, the three were removed, and
every release was checked again; the runs are on the repository's Actions page.
They ran for no later copy - those of 6.2.23 Alpha 1 to 6.3.3 started neither -
and both have been disabled there since 18 October 2026; a tag after 6.3.3
carries neither. The update path was not exposed: its feed is GitLab's,
and `SigstoreVerifier`, below, trusts the GitHub signer only under the old
repository's path, `Progressiverobot/hmailserver`, and only for certificates
issued before 18 September 2026, and a bundle those runs made meets neither.
Since 25 September 2026 GitLab CI has
replacements, not yet blocking - cppcheck, clang-tidy and Semgrep for CodeQL,
Trivy with OSV for the dependency checks - and [SECURITY.md](SECURITY.md) says
what runs.

Contents:

1. [What the software is](#1-what-the-software-is)
2. [Security requirements — the claims](#2-security-requirements--the-claims)
3. [Threat model](#3-threat-model)
4. [Trust boundaries](#4-trust-boundaries)
5. [Argument: secure design principles are applied](#5-argument-secure-design-principles-are-applied)
6. [Argument: common implementation weaknesses are countered](#6-argument-common-implementation-weaknesses-are-countered)
7. [Evidence](#7-evidence)
8. [Residual risk](#8-residual-risk)

---

1. What the software is
-----------------------

hMailServer is a mail server for Windows: an SMTP, IMAP and POP3
implementation, with a management COM API, a Control Panel GUI, and optional
REST API, metrics and web-services listeners. It stores mail on the local
filesystem and configuration and accounts in a SQL database. It talks to the
public internet by design, on ports that are open to anyone.

That last point sets the security posture: **the primary interfaces are
exposed to unauthenticated strangers**, and most of them must accept input
before any authentication has happened.

---

2. Security requirements — the claims
-------------------------------------

These are the properties the project claims and argues for below.

| # | Claim |
|---|---|
| **C1** | An unauthenticated network peer cannot execute code, corrupt memory, or crash the service through the mail protocols. |
| **C2** | An unauthenticated peer cannot read, modify or delete mail belonging to an account, nor authenticate as one. |
| **C3** | Credentials at rest cannot be practically recovered from a stolen database. |
| **C4** | Credentials in transit are not exposed to a network attacker, and are never sent to an unverified peer. |
| **C5** | Untrusted message content cannot alter the meaning of a database query, a filesystem path, or a protocol exchange with a third party. |
| **C6** | An authenticated ordinary user cannot reach another user's mail, nor gain administrative capability. |
| **C7** | A user can verify that the software they downloaded is the software the project published. |

What is explicitly **not** claimed is listed in
[SECURITY.md](SECURITY.md) under *Out of scope*, and repeated in
[Residual risk](#8-residual-risk).

---

3. Threat model
---------------

### Actors

| Actor | Position | Capability assumed |
|---|---|---|
| **A1 — Internet stranger** | Can open TCP connections to any listener | Arbitrary bytes, arbitrary volume, arbitrary timing, protocol non-compliance |
| **A2 — Sending MTA** | Delivers mail inbound | Everything A1 has, plus fully-controlled message content, headers and MIME structure |
| **A3 — Network attacker** | On-path between this server and a peer | Observe, modify, drop, replay; present a certificate; downgrade attempts |
| **A4 — Authenticated user** | Holds valid credentials for one account | Everything A2 has, plus authenticated IMAP/POP3/SMTP submission |
| **A5 — Malicious upstream** | Controls a DNS answer, a TLSA record, or a remote MTA | Poisoned resolution results, hostile TLS peer behaviour |
| **A6 — Local attacker** | Has a foothold on the host | Read files the service can read, observe process memory |

Administrators are **not** modelled as adversaries: an administrator can run
event scripts and change configuration by design, so administrative access is
equivalent to control of the service. This is stated in
[SECURITY.md](SECURITY.md) and is a deliberate boundary, not an
oversight.

### Assets

Mail in transit and at rest; account credentials; TLS private keys; the
configuration database; the availability of the service itself.

### Principal attack surfaces

- **Protocol parsers** (SMTP, IMAP, POP3) — reachable pre-authentication by A1
  and A2. The largest and most dangerous surface.
- **MIME and header parsing** — reachable by anyone who can send mail.
- **TLS layer and certificate handling** — reachable by A1, A3 and A5.
- **Anti-spam and anti-virus paths**, including DNS lookups and handoff to
  external scanners — driven by attacker-supplied content.
- **DNS resolution**, including DNSSEC, DANE and MTA-STS — influenced by A5.
- **The persistence layer**, reached indirectly with attacker-influenced data.
- **REST API, metrics and web-services listeners**, where enabled.

---

4. Trust boundaries
-------------------

Each boundary below is a place where data changes trust level and must be
checked.

```
                    ┌──────────────── UNTRUSTED ────────────────┐
   A1/A2/A3  ──▶    │  TCP + TLS termination                    │   ◀── B1
                    │  Server/Common/TCPIP                      │
                    └───────────────────┬───────────────────────┘
                                        │  B2: protocol grammar
                    ┌───────────────────▼───────────────────────┐
                    │  SMTP / IMAP / POP3 command parsers       │
                    │  MIME + header parsing                    │
                    └───────────────────┬───────────────────────┘
             B3: authentication         │
                    ┌───────────────────▼───────────────────────┐
                    │  Business objects (BO)  — per-account      │
                    └──────┬─────────────────────┬───────────────┘
          B4: query         │                     │  B5: process + filesystem
                    ┌───────▼────────┐    ┌───────▼───────────────┐
                    │  Persistence   │    │  ClamAV / SpamAssassin│
                    │  → SQL         │    │  event scripts, DNS   │
                    └────────────────┘    └───────────────────────┘

              B6: management plane — COM API ◀── Control Panel, scripts
```

- **B1 — Network to process.** Everything arriving is hostile until parsed.
  TLS is terminated here; peer verification happens *before* application data
  moves (see C4).
- **B2 — Bytes to protocol grammar.** Each command is accepted only in the
  form its grammar defines; anything else is rejected rather than
  interpreted generously.
- **B3 — Anonymous to authenticated.** Crossing it requires a credential
  check against a stored key-stretched hash. Below this line, an identity
  exists and every mail access is scoped to it.
- **B4 — Program to database.** Crossed only through parameterised
  statements; content never becomes query structure.
- **B5 — Process to external programs and the network.** Content handed to
  ClamAV, SpamAssassin, event scripts or DNS is untrusted in both directions,
  and every such call is time-bounded.
- **B6 — Management plane.** The COM API is the single seam through which
  configuration and management happen; it sits inside the administrative
  trust boundary described above.

---

5. Argument: secure design principles are applied
-------------------------------------------------

**Economy of mechanism / single seam.** All configuration and management flows
through one interface — the COM API in `Server/COM/` — used by the Control
Panel, the regression suite and external scripts alike. There is no second,
lightly-tested management path to audit. This is the layering documented in
[ARCHITECTURE.md](ARCHITECTURE.md).

**Complete mediation.** Mail access is mediated per account below B3; the
persistence layer is reached only through the business objects, not directly
from protocol code.

**Fail-safe defaults.** The server refuses to store a *new* secret under a
scheme that is not a password hash: configuring `PreferredHashAlgorithm` to
None, Blowfish or MD5 is logged as a misconfiguration and silently upgraded to
PBKDF2 rather than honoured. TLS peer verification is on by default, and a
verification failure fails the handshake rather than downgrading the
connection.

**Least privilege, explicitly bounded.** The administrative boundary is
declared rather than assumed: administrators can run scripts and change
configuration by design, and reports predicated on already having
administrator access are out of scope. Drawing the line publicly is itself a
design decision — it tells operators exactly what administrative access is
worth to an attacker.

**Defence in depth.** No single control is relied on. Transport security is
layered (STARTTLS and implicit TLS, DANE, MTA-STS, per-route TLS policy);
content is filtered by more than one mechanism; and memory-safety risk is
attacked from several directions at once (see §6).

**Separation of secrets from code and configuration.** TLS private keys are
files referenced by path; password hashes live in the database; the optional
server-wide pepper is an INI setting. No credential is compiled in.

**Cryptographic agility.** Password hashing (SHA256 / PBKDF2 / Argon2id), the
TLS cipher list, and the TLS key-exchange groups are all configurable, so a
broken primitive can be replaced by configuration rather than by waiting for a
release.

---

6. Argument: common implementation weaknesses are countered
-----------------------------------------------------------

### Injection (CWE-89)

SQL is **exclusively parameterised**, as a stated project rule in
[CONTRIBUTING.md](CONTRIBUTING.md): *"Use parameterised SQL
exclusively — never build SQL strings manually."* Message and header content
therefore never becomes query structure. The rule is enforced at review, and
the persistence layer offers parameterised interfaces as the path of least
resistance.

### Memory corruption (CWE-119, CWE-787, CWE-125)

This is C++, so this class cannot be argued away — it is attacked from four
directions:

- **Compiler-enforced hygiene.** The native build treats warnings as errors
  (`TreatWarningAsError`, MSVC `/WX`), so a new warning stops the build.
- **Platform hardening.** `/GS` buffer security checks, DEP (`/NXCOMPAT`) and
  ASLR (`/DYNAMICBASE`) are active on the shipped x64 binaries.
- **Static analysis.** CodeQL analyses the C# tools on every push and pull
  request, and the C++ server on every push to master, weekly and on demand (not
  on pull requests - a change to the protocol parsers is analysed by dispatching
  the workflow against the branch before merging).
- **Fuzzing.** A libFuzzer harness suite lives under `fuzz/`, with a seed corpus
  regenerated from the real test messages (`make-corpus.ps1`; not committed), a
  MIME token dictionary and a committed regression directory of fixed findings,
  aimed at exactly the pre-authentication parsers that A1 and A2 can reach.

### Protocol-level smuggling (CWE-444)

SMTP line-terminator handling is deliberately restrictive. Bare-LF spellings
of the terminator are accepted only when *Allow incorrect line endings* is
enabled, and even then are recognised only at the very end of the buffer with
anything behind one discarded — the CVE-2023-51764 SMTP-smuggling rule. This
behaviour is pinned by its own regression test, so it cannot be relaxed by
accident.

### Broken authentication and credential storage (CWE-287, CWE-916)

Passwords are stored as per-user salted, key-stretched hashes — PBKDF2 by
default, Argon2id available — with salts from OpenSSL `RAND_bytes` and an
optional HMAC pepper mixed into Argon2id hashes. A minimum acceptable scheme
can be configured so legacy weak hashes stop being accepted for
authentication. SCRAM-SHA-256 is supported, and TOTP is available as a second
factor.

### Weak randomness (CWE-338)

All security-relevant randomness comes from OpenSSL's CSPRNG (`RAND_bytes`),
with every call checked for failure: password-hash salts, SCRAM nonces, TLS
session-ticket keys and IVs, app passwords, TOTP secrets, REST API key
material and DNS/DNSSEC transaction IDs. Non-cryptographic generators appear
only in MIME part-boundary strings and OpenTelemetry trace identifiers,
neither of which is a security mechanism. A previous use of `rand()` for a
DNS-adjacent value was identified and replaced.

### Improper certificate validation (CWE-295)

Outbound TLS sets `verify_peer` with `verify_fail_if_no_peer_cert` and
installs a verification callback that checks the chain and the expected
hostname, with a DANE verifier where a TLSA record applies. Because this is
installed before the handshake completes, credentials cannot be sent to an
unverified peer — which is claim C4.

### Uncontrolled resource consumption (CWE-400)

Availability is treated as a security property, not a performance one. Several
unbounded waits were found and fixed as defects in this fork — database-pool
acquisition, event scripts, the custom virus scanner, the delivery-side ClamAV
read/write timeout, and spam-test DNS lookups, which previously had no
application-level timeout at all. Every crossing of boundary B5 is now
time-bounded.

### Supply-chain and integrity weaknesses (CWE-494)

Every release asset is signed with Sigstore cosign (keyless) and verified by
the workflow before upload, giving claim C7. Dependencies are enumerated in an
SBOM, monitored by Dependabot, and gated by dependency review. Every vendored
binary is inventoried with a SHA-256 and its provenance, and an unlisted or
changed binary fails the build — closing the "a DLL appeared and nobody
noticed" gap that a diff review cannot catch.

New in 6.2.28, the server's own update path applies the same verification. `UpdateChecker` reads the release feed and
`SigstoreVerifier` checks the installer against its `.cosign.bundle` in-process -
the file's SHA-256 is the one signed and the one Rekor recorded, the certificate
chains to the embedded Sigstore trust root, and its identity must be one of this
project's two release signers, whole: the `sign-release.yml` workflow issued by
GitHub's token service, which signed the releases from 6.2.19 to 6.3.3 and is trusted
only for certificates issued before the account's suspension on 18 September
2026, or this project's GitLab CI pipeline issued by GitLab's, for the `v*` tag
of the release being installed, which signs the releases after, from 6.3.5 -
at the path the project began at, which signed 6.3.5, and at the path of the
group `hmailserver` it moved to on 29 September 2026, which signs every release
from 6.3.6 and where the certificate must also record the project's id, so that
the servers built before the move take what is signed after it. A GitHub-signed file is installed only as a release up to
6.3.3, and on Windows the installer's own version resource must name the release
it is installed as - so a substituted installer, or a genuine older one offered
as a newer release, is refused before it is run. On Windows the `Updates` folder
that holds it admits changes from SYSTEM, the Administrators and the service
alone, and the update helper runs the installer, and the rollback image, only
with the SHA-256 the server read when it last checked them, through a handle it
holds until they have finished. The feed it reads is
https://updates.progressiverobot.com, served by GitLab Pages from the project's
releases (`hmailserver/docs/UpdateFeed.md`).

---

7. Evidence
-----------

| Claim | Where the evidence is |
|---|---|
| C1 | `fuzz/` harnesses; the CodeQL workflow up to 18 September 2026, and the *static-analysis* and *semgrep* jobs of `.gitlab-ci.yml` since; `/WX` in `hMailServer.vcxproj`; the regression suite under `hmailserver/test/RegressionTests` |
| C2 | Authentication path and per-account scoping in `Server/Common/BO/`; regression tests for authentication |
| C3 | `Server/Common/Util/Hashing/HashCreator.cpp`; `IniFileSettings.cpp` hash-scheme handling |
| C4 | `Server/Common/TCPIP/TCPConnection.cpp` verify mode and callbacks; `SslContextInitializer.cpp` |
| C5 | Parameterised-SQL rule in `CONTRIBUTING.md`; persistence layer; SMTP terminator tests |
| C6 | Account scoping in the BO layer; administrative boundary in `SECURITY.md` |
| C7 | Up to 6.3.3, the GitHub workflows `sign-release.yml`, `verify-binary-provenance.yml` and `sbom.yml` (git history, `v6.3.3`); from 6.3.5, the release jobs of `.gitlab-ci.yml` (`build/ci/release-chain.py`, `release-sign.sh`, `release-verify.sh`); `SigstoreVerifier.cpp`. That a published binary corresponds to the published source: the reproducible builds, RELEASE.md step 8 and the `linux-repro` job (`build/ci/linux-repro.sh`; [ReproducibleBuilds.md](hmailserver/docs/ReproducibleBuilds.md)) |

Release validation is not a smoke test: the full regression suite runs against
the exact binary being shipped, with live SpamAssassin and ClamAV (real EICAR
detection), DMARC, SPF and DKIM evaluated against a DNS zone the suite serves
itself (`Shared/SuiteDns.cs`, so a run cannot depend on the public DNS), and TLS
1.2 and 1.3 handshakes end to end. Nothing is mocked except the resolver, and
nothing is skipped.

---

8. Residual risk
----------------

Stated plainly, because an assurance case that claims no residual risk is not
credible.

- **Memory safety is mitigated, not guaranteed.** This is a large C++
  codebase with a pre-authentication attack surface. The controls in §6 raise
  the cost of exploitation; they do not make it impossible.
- **Coverage is below the bar.** The native server's line coverage over the
  whole Windows regression suite was 73.48% on 15 September 2026 (67,790 of
  92,260 lines under `hmailserver/source/Server`), by OpenCppCoverage 0.9.9
  against the 6.3.3 shipping binary run as a console server
  ([CHANGELOG.md](CHANGELOG.md), *6.3.4*, *Build and tests*;
  `test_statement_coverage80` in [Roadmap.md](Roadmap.md)) - lines only, the
  tool having no branch mode. Branches are counted on Linux: on 26 September
  2026 the Linux build under clang's source-based coverage, driven by the
  Linux regression suite and the tester programs, reached 49.01% of its lines,
  47.23% of its regions and 38.87% of its branches
  (`build/ci/linux-coverage.sh`; the suite in two runs whose counters were
  merged - the whole of it before 29 fixtures were linked into it that day,
  and those fixtures on their own). That is a floor rather than the Windows
  figure's equal: the Linux suite runs only the fixtures its REST shims can
  drive, the Linux build has no COM layer, and llvm-cov counts every line of a
  function where OpenCppCoverage counts the lines an optimised build keeps.
  Every figure is below the 80% the OpenSSF silver level asks of statements,
  and the branch figure far below the 80% gold asks of branches. Both scripts
  are in the repository - `build/coverage-windows.ps1`, and
  `build/ci/linux-coverage.sh`, which the `linux-coverage` job runs on a
  weekly schedule the owner has yet to create - but neither is a release step,
  the job has not run on GitLab and the Windows script has not run from the
  tree, so the figures are not yet repeated for each release. The .NET Control
  Panel's coverage was collected by CI on GitHub (`ci.yml`) until 18 September
  2026 and is not collected on GitLab yet, nor on GitHub since then.
- **Reproducibility has been shown on the project's own machines only.**
  ([hmailserver/docs/ReproducibleBuilds.md](hmailserver/docs/ReproducibleBuilds.md)
  has the procedure and the evidence.) Since 22 August 2026 two clean Release
  builds of the same tree produce a byte-identical `hMailServer.exe`
  (`/Brepro`, `/d1trimfile`, `/pdbaltpath`, `OPENSSL_NO_FILENAMES`; RELEASE.md
  step 8 repeats the check for every release and the notes carry the hash); on
  26 September 2026 two builds of `40c7d3e0c`, the commit v6.3.4 was first
  tagged at that day, were both `c42e31bb...`. The same day a build of that commit from
  a second checkout at another path was *not* identical: `/d1trimfile`
  trimmed only the project file's directory, and sixteen source paths and 31
  Boost header paths stayed absolute. With the checkout's root and the
  libraries' root trimmed, two checkouts at two paths build the same
  executable (`53f9801f...`), and step 8 now builds once more in another
  checkout. 6.3.4, built before the change, reproduces only from its own
  paths. On Linux the `linux-repro` job builds `linux-build`'s commit a
  second time from another path with other file modes, in another build
  directory, umask and time zone, and requires the binary and the x86-64
  `.deb` and `.rpm` to be byte for byte identical before a release (the
  packages became so the same day, when it found their directories' modes
  following the umask, the `.deb`'s maintainer scripts the checkout's modes
  and the `.rpm` stamping the clock); the AArch64 `.deb` and `.rpm` a release
  carries from 6.3.6 are built once, by the package jobs, and not compared;
  the image's Ubuntu packages are the archive's of the day it was built, so it
  pins the build, not the toolchain's history. What has not happened is an
  independent rebuild by someone outside the project, with an independently
  built OpenSSL, Boost and libpq, so a third party's ability to confirm a
  released binary still rests on matching this toolchain exactly. Artifact
  signatures establish *who published* a binary; the hash lets anyone with
  the toolchain check that it corresponds to the source.
- **Control Flow Guard is on, and its cost is small but real.** Since 6.2.23
  Alpha 2 the server is compiled and linked with `/guard:cf` (Release and
  Debug), so an indirect call through a corrupted function pointer or vtable -
  the natural target of a memory-safety defect in the SMTP, IMAP, POP3 or MIME
  parsers - terminates the process rather than transferring control. The cost
  was measured rather than assumed: the full 1,838-test regression gate on
  the CFG build took 31 minutes on the build machine, against the 55 or so
  the previous release's gates took on the same machine, so whatever CFG
  costs on the indirect-call-heavy MIME path is below the run-to-run
  variance of the suite. The executable grew by about 97 KB of guard tables.
  CFG is a mitigation, not a boundary: a defect that overwrites data rather
  than a pointer is unaffected.
- **Bus factor is still effectively 1.** Since September 2026 the project has
  two maintainers, one from Progressive Robot Ltd and one independent of it,
  and from the first change after 6.3.4 the rule is that every merge request
  is approved by one before the other merges it; but of the nine batches
  landed by 14 October 2026 one had that approval and eight landed on the
  owner's override, and since 13 October 2026 the second holds no role in the
  GitLab project. Release tagging is the owner's alone (the only person with
  GitLab's Maintainer role, and the one key `.gitlab/allowed_signers` may
  hold), security triage rests on him, and so far only one person has a record
  of changes to the codebase —
  see [GOVERNANCE.md](GOVERNANCE.md#continuity). A slow security response
  remains a realistic failure mode.
- **Administrators are inside the boundary.** Anyone who can administer the
  server can run code on it. Operators must treat administrative credentials
  as equivalent to host credentials.
- **Operator-chosen weakening is possible.** The server permits plaintext
  listeners and legacy hash schemes for compatibility. It warns, but it obeys.

Reviewing this document
-----------------------

This assurance case should be revisited whenever the threat model changes — a
new listener, a new external integration, a new authentication mechanism — and
at minimum reviewed against the codebase once a year. Corrections are welcome
through the normal merge-request process described in
[CONTRIBUTING.md](CONTRIBUTING.md), or as a pull request on GitHub, which a
maintainer lands through GitLab as a `batch*` branch and a merge request,
keeping its author and sign-off.
