The Control Deck and the webmail: the framework decision
=========================================================

**Decided 12 October 2026; nothing of it is built yet: the rebuild it plans is
for 6.4.** This is row A1 of Programme A, the rebuild of the Control Deck and
the webmail on a strongly typed framework that escapes by construction
(Roadmap3 row 115). 6.3.7 ships row A1 itself - this decision, the
threat model and the authorisation matrix with its checker - and nothing of the
rebuild. It records the criteria, the candidates and their scores, the decision
and why, what was rejected and why, the migration plan for rows A2 to A4, the
build, and how the pages are to be served. The threat model the rebuild answers
to is `hmailserver/docs/WebThreatModel.md`, and the authorisation of every
route the pages call is `hmailserver/docs/WebAuthorisationMatrix.md`,
which `build/generate-auth-matrix.py` writes from the code. What it counts of
today's pages, and its file and line references, are the tree's at 6e1a336c5
(15 October 2026); the measurements are the evaluations' (Evidence, below).

The decision
------------

**Preact, written in strict TypeScript with JSX and bundled by esbuild alone,
for the Control Deck, the webmail and portal, and the protected-message page.**
Two frameworks ran under the policies the pages are served with today, and
under the stricter policy they are to end with, while keeping their raw-HTML
escape refused by the browser: Preact and React 19. Preact is chosen over React
for its size (6.3-6.6 KB of runtime gzipped against 68-70 KB), its boot time and
row swaps on a slow machine, and a bundle small enough to be reviewed as a diff
at every upgrade. It is one runtime package of no dependencies; its bundle is
built from three installed packages, all of them attested, and its output was
byte-identical from four different environments. The type checker, TypeScript,
is the build's one unattested part, and is kept out of the bundle's path (The
build, below).

The release line: **A2 starts on Preact 10.29.x** (10.29.8, patched on 1 August
2026; the 10.x line ran from October 2019 to September 2026). Preact 11.0 was
published on 30 September 2026. The three evaluations made their browser,
harness and size measurements on 11.0.1; the policy cells were run again on
10.29.8 (below), and the harness's DOM was found to need more on 10.29 than on
11 (Phase 0). So the A2 spike runs the evaluations' measurements on 10.29.8
before the first view moves, and the policy matrix and the harnesses on 11.0.x
too; the move to 11 is made when it has had two months
of patch releases (not before 30 November 2026), passes the same matrix, and a
Renovate merge request for it is approved on the dashboard. The programming
model is the same on both lines.

The fallback, if a measurement in A2 rules Preact out: **no framework**, with
the same toolchain, the same phases and a project-owned typed `h()` in place of
JSX. If a library that only React has becomes necessary, **React 19** with the
same toolchain; nothing else in this plan changes.

What is being rebuilt
---------------------

| Page | Today | Served by |
|---|---|---|
| Control Deck | `hmailserver/installation/WebAdmin/index.html`: 631,298 bytes, 9,033 lines, one inline script. Views drawn by assigning HTML strings: 94 `innerHTML` assignments, `escHtml(` 912 times. 40 views, 245 distinct button actions, one `api()` client. 20 catalogues beside English. | Read from disk on every request (`RestApiServer::HandleWebAdminPage_`, RestApiServer.cpp:11641); its one inline script allowed by its SHA-256 (RestApiServer.cpp:11675) |
| Webmail and portal | `Portal.html` (188,047 bytes), `Portal.js` (860,446 bytes, 14,197 lines of ES5), the S/MIME, OpenPGP and Markdown modules, the service worker. Screens built with DOM calls (`createElement`, `textContent`); 3 `innerHTML`. 23 catalogues, three right to left. | Compiled into the binary by `build/generate-portal-page.py` (`PortalPageData.cpp`); `script-src 'self'` (RestApiPortal.cpp:50) |
| Protected-message page | `PortalProtected.html` and `.js`, 15 KB | Compiled in with the portal |
| Seven script-free pages from six builders, the message-HTML frame, the help | C++ literals and JSON | Unchanged by this programme (below) |

There is no third-party JavaScript in the tree today and no `package.json`. The
rebuild adds the project's first npm dependency, so most of what this decision
weighs is what that first step costs and how it is held.

The tests that exist and must keep running: `build/deck-script-test.js` (about
1,210 checks) and `build/portal-script-test.js` (about 1,140) run each page's
script in Node against a small DOM and a recorded server; `trusted-types-test.js`
runs the Deck's policy, and it and `check-deck-script.py` find that policy by
its comment heading and its exact text (Phase 0 keeps both); the S/MIME and
OpenPGP modules are checked against OpenSSL and GnuPG; `check-deck-parity.py`
measures the Deck against the desktop
Control Panel (`DeckParity.md`: 517 written, 507 writable over REST, 507 reached
by a view, 10 missing). There is no test in a real browser today, and no
automated accessibility check.

The criteria
------------

Three evaluations judged the candidates, each through one lens and without
seeing the others: security and supply chain; engineering and migration;
long-term maintenance and operations. Each wrote its own weighted criteria.

**Gates, which a candidate must pass to be eligible at all.** They come from
Roadmap3 row 115 and from the way the pages are served:

| Gate | Requirement |
|---|---|
| G1 | Runs with `script-src` limited to hashes: no `'self'` for the Deck (this origin serves files whose type a sender chose), no `'unsafe-inline'`, no `'unsafe-eval'`, no `'wasm-unsafe-eval'`. |
| G2 | Runs with `require-trusted-types-for 'script'` under the policy names the pages are served with - the Deck's `default` alone today, `'none'` at the end - and without a pass-through `default` policy; and needs no framework-named policy that would let its own raw-HTML escape past Trusted Types. |
| G3 | Installs and builds with nothing fetched from GitHub (`.gitlab/ci/no-github.yml`), measured by DNS rather than read from documentation. |

**The scored criteria**, in short (each evaluation gives them in full):

| Lens | Criteria and weights |
|---|---|
| Security | Strict CSP fit 15; Trusted Types 20; escaping by construction 15; size and provenance of the tree 20; nothing from GitHub, lock integrity 5; SBOM and a build reproducible three ways 5; Renovate and update burden 5; security record and support 15 |
| Engineering | Fit with how the pages are served 20; incremental migration 15; typed declarative escaping end state 15; tests installable without GitHub or a browser download 10; supply chain and reproducibility 15; size and low-end performance 10; accessibility 10; languages, offline and longevity 5 |
| Maintenance | Upgrades over five years 20; support lifetime 10; community and governance 10; dependency surface 15; hermetic build 10; fit with the packaging 10; CI cost 5; licence 5; operator reach 5; ownership and people 10 |

**The owner's requirements**, which every lens measured: nothing fetched from
GitHub when the tooling installs or builds; a front-end build reproducible three
ways; an SBOM; Renovate coverage; an air-gapped build; and "a strongly typed,
auto-escaping framework chosen by a written evaluation, not by taste".

How it was measured: the same probe app in every candidate, built in throwaway
containers on the project's Linux builder with GitHub's names answered by
nothing, served as the Deck is served (one inline script allowed by its hash)
under the Deck's policy exactly as `RestApiServer.cpp:11675` sends it and under
stricter ones, in Debian's Chromium 155 and Firefox ESR 153.4 with nothing
downloaded. Hostile values in text, attribute and URL positions; each
framework's raw-HTML escape fed markup on purpose; lock files, install scripts,
provenance and licences read; the output hashed from several environments.

The candidates and the scores
-----------------------------

Scores out of 10 on each lens. "n.e." is not eligible on that lens (it failed
one of that lens's own gates). The gates column is this decision's G1 to G3 as
the table above states them, which no single lens applied whole: the security
lens scored Lit and Vue as eligible, its own gates being met with the
framework's policy name admitted.

| Candidate | Gates G1-G3 | Security | Engineering | Maintenance | Mean | Verdict |
|---|---|---|---|---|---|---|
| **Preact + TypeScript + esbuild** | pass | **8.8** | **8.75** | 8.10 | **8.55** | **Chosen** |
| No framework (TypeScript modules, a typed DOM helper, esbuild) | pass | 8.6 | 8.40 | 8.35 | 8.45 | Fallback |
| React 19 + TypeScript + esbuild | pass | 8.7 | 8.00 | 7.95 | 8.22 | Second fallback |
| Lit 3 + esbuild | **fails G2** | 7.9 | 6.30 | **8.65** | 7.62 | Rejected |
| Svelte 5 | **fails G2** | 7.8 | 6.70 | 6.30 | 6.93 | Rejected |
| Vue 3 + Vite | **fails G2** | 7.0 | 6.75 | 6.85 | 6.87 | Rejected |
| Angular 22 | pass | 6.7 | 6.45 | 6.30 | 6.48 | Rejected |
| SolidJS | **fails G2** | 5.3 n.e. | 5.65 | 5.90 | 5.62 | Not eligible |
| htmx, server-rendered | **fails G2** | 5.0 n.e. | 3.50 | 6.65 (n.e. for the webmail) | 5.05 | Not eligible |
| Blazor WebAssembly | **fails G1 and G2** | 4.5 n.e. | 3.25 | 6.10 | 4.62 | Not eligible |

The mean is shown for orientation only. The decision is not the mean: it is the
gates first, then the lens that owns each disputed property (next section).

**The security scores of the three TypeScript candidates are re-scored on one
tree.** The security evaluation measured its criterion C4 (size and provenance
of the tree, weight 20) for Preact and for no framework on a tree of esbuild
alone ("three installed, all three attested"), and for React with TypeScript.
`typescript` 7.0.2 and `@typescript/typescript-linux-x64` carry no provenance
attestation (measured on server2, 13 October 2026), so with TypeScript the
Preact build installs five packages of which three are attested, and the
framework-free one four of which two are. On the evaluation's own scale (React's
ten packages with five attested scored 7, htmx's three with two attested 8,
Preact's three of three 9, no framework's two of two 10), Preact's C4 becomes 8
and no framework's 9, each a point lower; React's 7 already had TypeScript in
it. The totals are then Preact 8.8, React 8.7 and no framework 8.6, and the
order the security lens gives is unchanged.

### What each measurement found that decides

| Measurement | Preact | No framework | React | Lit | Vue | Svelte | Angular |
|---|---|---|---|---|---|---|---|
| Runs under the Deck's policy as served (`trusted-types default`) | yes | yes | yes | **no**: `createPolicy('lit-html')` refused, not caught | the probe yes; **no** with one realistic static help card (its compiler emits an `innerHTML` string) | **no** | yes |
| Runs under `trusted-types 'none'` | yes | yes | yes | no | no (as above) | no | yes |
| Raw-HTML escape, under the policy that lets it start | refused, render fails closed | refused | refused | **`unsafeHTML` injects** through Lit's own policy | **`v-html` injects** through Vue's own policy | `{@html}` refused | sanitised; the bypass refusable |
| Template typos caught by the checker | yes (`tsc` 7) | yes | yes | property bindings **not** checked (lit-analyzer last published January 2024) | yes, but vue-tsc does not start on TypeScript 7 | yes, pins TypeScript 6 | yes |
| Installed packages for the production build | 5 | 4 | 10 | 8-10 | 35 | 21-46 | 228 |
| Byte-identical output from every environment tried | yes | yes | yes | yes | yes | yes | not on Node 20 |
| Runtime gzipped (probe app) | 6.3-6.6 KB | 1.1 KB | 68-70 KB | 7.4 KB | 25 KB | 14 KB | 38-40 KB |
| Stable releases in 12 months | 18-23 | - | 25 | 2 | 21 | 110 | 102 |

The Lit and Preact rows of the first three lines were measured by two of the
evaluations independently, in Chromium and in Firefox, and once more by the
decider before deciding: on the project's Linux builder, in the harnesses CI
image, in Debian's Chromium 155, with each probe served as the Deck is served
under today's policy, under today's policy beside a deny-list `default` policy,
under `trusted-types 'none'`, and with `lit-html` admitted by name. That re-check
ran Preact 11.0.1, and its `default` policy refused eleven elements where the
Deck's refuses 26 (index.html:533). Lit rendered only when `lit-html` was
admitted, and then its `unsafeHTML` put markup into the page; Preact rendered
under all four, and its `dangerouslySetInnerHTML` was refused everywhere except
beside the permissive `default` policy, which lets through any element not on
its deny-list. This decision's review ran the Preact cells again on 10.29.8,
beside the Deck's own policy as index.html makes it, and found the same: it
rendered under the Deck's policy as served, beside the Deck's `default` policy
and under `'none'`, and its raw escape was refused but beside the `default`
policy. That last cell is why the endgame removes the `default` policy.

Where the lenses disagree, and which wins
-----------------------------------------

Security and engineering chose Preact; maintenance chose Lit, with Preact third
by 0.55. **Security and engineering win, for four reasons.**

1. **Lit fails a gate the maintenance lens did not test.** The maintenance
   evaluation ran no browser ("No browser test was run" is in its own limits),
   and its recommendation includes adding `lit-html` to the Deck's
   `trusted-types` directive. Two browser measurements and the decider's own
   show what that costs: Lit does not start under the policy the Deck is served
   with today, and once its name is admitted, `unsafeHTML` passes Trusted Types
   through Lit's own pass-through policy. Trusted Types would then fence nothing
   Lit writes, and the only guard left on the raw path is a lint rule. The row
   exists because "an attacker who can only send mail would try to run script
   as the administrator"; the browser-enforced backstop against that is the
   property the security lens weighs most, and it is the security lens's to
   decide.
2. **"Strongly typed" is a requirement, not a preference.** Lit's property
   bindings are strings inside the template, so a misspelt property is not an
   error (measured); the analyser that would check them has not been published
   since January 2024. Preact's JSX is checked by plain `tsc`, including
   TypeScript 7.
3. **The mechanical-migration argument is weaker than it looks.** Lit's
   `` html`…` `` looks like today's Deck templates with `escHtml`, which makes a
   port look mechanical. But the webmail, the larger half, is written with DOM
   calls (`createElement` on 169 lines of Portal.js, `textContent` on 223),
   which map to JSX or `h()` directly; and a mechanical port of the Deck would
   carry over string-building habits that the rebuild exists to end. Shadow DOM,
   Lit's default, would also break the `id`-based ARIA the webmail's message
   list depends on (`aria-activedescendant`), and `styleMap`'s first render
   writes a `style` attribute that a hash-only `style-src` refuses.
4. **What the maintenance lens rightly weighs is answered without Lit.** Its
   concerns are cadence, governance and owning too much. Preact is one package
   with no dependencies and about 18-23 releases a year (about 34 Renovate
   merge requests a year with esbuild and TypeScript, against Lit's 18); its
   10.x line lasted seven years; and the committed, readable bundle makes every
   upgrade a reviewable diff. Volunteer governance and six npm maintainers are
   the remaining risk, and the exit is cheap: the same views move to React by a
   change of import, or to no framework by a typed `h()`.

Where the lenses agree, the decision follows them: no framework is second on all
three and is the fallback; Svelte, Vue, Angular, SolidJS, htmx and Blazor are
rejected on all three.

What was rejected and why
-------------------------

| Candidate | Why not |
|---|---|
| No framework | Not rejected: the fallback. Not chosen because the project would own the escaping-by-construction renderer - keyed lists, component state, focus - several thousand lines of security-critical code nobody outside has reviewed, which is the kind of code the rebuild exists to replace; and the row asks for a framework. |
| React 19 | Passes the gates and serves exactly as Preact does, and blocks `javascript:` addresses itself, but is 11 times the size (68-70 KB gzipped), boots twice as slowly at 4x CPU slowdown and swaps two rows eight times as slowly; and of the 10 packages its build installs, 5 carry a provenance attestation (the chosen build's are 3 of 5 with TypeScript, 3 of 3 in the install the bundle is built from). The second fallback, if a React-only library is ever needed. |
| Lit 3 | Fails G2 both ways: it does not start under the policy name the Deck is served with, and admitting `lit-html` lets `unsafeHTML` past Trusted Types. Untyped property bindings; shadow DOM by default; `styleMap` writes style attributes. Best record of any framework (no advisory ever), which is why it was close. |
| Svelte 5 | Fails G2: it does not start under the policy name the Deck is served with (`svelte-trusted-html` refused, not caught). With its name admitted it behaves well (`{@html}` refused), but 110 releases a year, svelte-check pinned to TypeScript 6, and a rewrite into `.svelte` files rather than a port. |
| Vue 3 | Fails G2: its compiler turns any run of 20 static nodes into an `innerHTML` string, so a real page dies under today's policy and under `'none'`; kept alive only by `hoistStatic: false` in every build or by admitting `vue`, which lets `v-html` through. vue-tsc does not start on TypeScript 7. |
| Angular 22 | The best Trusted Types design measured, and the worst supply chain: 228 installed packages, 214 MB, native install scripts, no npm provenance on any release, 17 advisories in 2025-2026 of which ten are sanitiser or template bypasses, a major every six months, refuses Node 20, and it wants to own the page. |
| SolidJS | Fails G2: its templates are `innerHTML` strings with no policy, so it runs only behind a pass-through `default` policy. One npm maintainer, no provenance, 2.0 a breaking release candidate. |
| htmx | Fails G2 (`parseHTMLUnsafe` with no policy). Moves every view into C++ string building with no auto-escaping template engine, and makes the webmail's offline reading, search and outbox impossible. |
| Blazor WebAssembly | Fails G1 and G2: needs `'wasm-unsafe-eval'`, and its renderer's `innerHTML` is refused by Trusted Types. 2.7-2.8 MB of runtime, not reproducible by default. |
| Elm, Alpine.js, Mithril, server-rendering meta-frameworks | Excluded before scoring: Elm's package tool fetches from GitHub by design; Alpine needs `'unsafe-eval'` outside its CSP build; Mithril has one release a year and a 3.0 that has stayed on `next`; SSR frameworks need a Node runtime on every mail server. |

The security envelope the rebuild is held to
--------------------------------------------

The framework is a small part of the protection. These rules hold whichever
framework ends up in the bundle, and each has a check.

1. **Headers, at the end of the programme.**
   - The Deck: `default-src 'none'; script-src 'sha256-<bundle>'; style-src
     'sha256-<stylesheet>'; img-src 'self'; connect-src 'self'; form-action
     'self'; frame-ancestors 'none'; base-uri 'none';
     require-trusted-types-for 'script'; trusted-types 'none'`, plus the CSP
     report directives of A4. `script-src` never names `'self'`. No `default`
     policy at all: every HTML sink is then a bug the browser refuses. Styles
     through the CSSOM only (Preact's `style` prop does this; measured under
     `'none'` with styles applied); whether `style-src 'unsafe-inline'` can go
     is measured in the A2 spike.
   - The webmail: `default-src 'none'; script-src 'self'; style-src
     'sha256-<stylesheet>'; img-src 'self' data: blob:; connect-src 'self';
     frame-src 'self' blob:; manifest-src 'self'; worker-src 'self';
     form-action 'none'; frame-ancestors 'none'; base-uri 'none';
     require-trusted-types-for 'script'; trusted-types <the one policy>`, plus
     the CSP report directives of A4: today's header (RestApiPortal.cpp:50) with
     the style and Trusted Types directives tightened, and not the Deck's, which
     would block the message-HTML frame and the PDF preview (`blob:` frames),
     the `data:` and `blob:` images, the worker and the manifest the share target
     needs. `script-src 'self'` stays (its files are the server's own fixed
     routes), and there is **one named policy, not `default`**,
     created first in one module, with `createHTML` for `DOMParser` of message
     HTML only and `createScriptURL` for the worker's and the OpenPGP module's
     addresses only. `check-portal-script.py` holds the policy to that one
     module. Message HTML stays in the sandboxed frame; there is no sanitiser
     in the page.
2. **One bundle a page, committed, readable and reproduced.** The built files
   are committed as generated sources, as `PortalPageData.cpp` is today, so the
   Windows installer, the Linux packages, Nix and the distributions' offline
   source builds never run npm. Not minified (identifiers and whitespace kept):
   the server compresses responses on the routes `CompressibleRoute_` names
   (RestApiServer.cpp:1166-1206; A3 names the new ones), a readable bundle
   makes every framework upgrade a reviewable diff, and lintian's
   `source-is-missing` stays a false positive. A CI job rebuilds from the lock file and fails on any byte of
   difference.
3. **Lock-file rules**, held by a Python checker in the `checkers` job:
   `package-lock.json` version 3; every `resolved` on
   `https://registry.npmjs.org/`; every `integrity` sha512; no git, file or
   tarball dependencies; exact versions in `package.json`; a `version` in
   `package.json` (the server's; `npm sbom` refuses a package without one); and
   a provenance attestation for every package but an allow-list that names
   `typescript` and its `@typescript/typescript-*` platform packages, which
   publish none. Install with `npm ci --ignore-scripts`; the byte-identical
   bundle was measured with install scripts off. `.npmrc`: `ignore-scripts=true`,
   `fund=false`, `audit=false`. `npm audit signatures` runs in the job (it needs
   only the registry and `tuf-repo-cdn.sigstore.dev`).
4. **The tree kept small on purpose.** The checker fails when the lock's
   package count passes a recorded number, so that each new package is a
   decision. Production: `preact` and `esbuild` as dependencies, `typescript`
   the one development dependency, and their platform binaries; the bundle is
   built from `npm ci --omit=dev`, which installs three packages, all attested
   (measured: preact, esbuild and @esbuild/linux-x64, no TypeScript tarball
   fetched). Tests are a second lock (below).
5. **A raw-HTML checker** in the style of `check-deck-script.py`. Outside the
   one webmail module it refuses `dangerouslySetInnerHTML`, `innerHTML`,
   `outerHTML`, `insertAdjacentHTML`, `document.write`, `DOMParser`,
   `createContextualFragment`, `srcdoc`, `setAttribute('style', …)` and
   `new Function`. URL attributes go through one `safeUrl()` helper (`https:`,
   `http:`, `mailto:`, a relative path, `#`). API data is rendered as strings
   and numbers only, never spread into the tree as objects - the class of
   CVE-2026-22028, Preact's JSON VNode injection (fixed in 10.26.10, 10.27.3
   and 10.28.2; 10.29.8 and 11.0.1 come after it).
6. **The Deck compiled into the binary**, as the webmail is (A2, below). Today
   the Deck's script hash is computed from whatever file is on disk
   (RestApiServer.cpp:11641-11675), so the policy defends the page against
   injection but not against a changed file; see WebThreatModel.md, W-4.
7. **The no-GitHub list** gains `release-assets.githubusercontent.com` and
   `github-releases.githubusercontent.com`, which resolve today and serve
   GitHub's release downloads, and every job that runs `npm ci` takes `.no-github`
   as its first `before_script` line.

The build
---------

**Layout.** `hmailserver/source/WebUi/`: `package.json` (version = the
server's), `package-lock.json`, `.npmrc`, `tsconfig.json` (strict), `src/deck/`,
`src/webmail/`, `src/protected/`, `src/shared/` (the catalogue functions `t()`,
`tf()` and `K()`, the REST client, the Markdown renderer, which the Deck then
bundles instead of keeping a hand copy), and `tests/` with its own lock. The
S/MIME and OpenPGP modules move in unchanged in logic, with types added.

**The toolchain.** `preact` (10.29.8 to start), `esbuild` 0.28.2 and `typescript`
7.0.2, exact versions, from registry.npmjs.org only. Not Vite: `@preact/preset-vite`
brings Babel and 90 packages for nothing esbuild does not do. `tsc --noEmit`
type-checks and esbuild strips the types itself, so TypeScript writes none of
the shipped bytes - but it is a native program from a release with no
provenance attestation, and run beside the build it could write the sources,
`node_modules/esbuild` or the bundle. So it is kept out of that path by how it
is run, not by its role: the bundle is built from an install without it, and
the type check runs apart from the build (the generator, below). The Deck's
bundle is built with `--format=iife` (Preact 11 ships ES modules only; the IIFE
keeps the Deck one classic inline script, which the existing harness runs in
Node's `vm`).

**The generator.** `build/generate-webui.py` (new, in batch PA-1) runs
`npm ci --ignore-scripts --omit=dev` and esbuild, in an install with no
TypeScript in it, and writes the committed outputs; the type check is a step of
its own (`--typecheck`: a full `npm ci --ignore-scripts` into a separate
directory and `tsc --noEmit` over a read-only checkout), which CI runs as its
own job, so that nothing TypeScript runs shares a file system the bundle is
built on. The committed outputs: `WebAdmin/index.html` until the Deck is
compiled in, then `Common/Util/DeckPageData.cpp`; `Portal.html`, `Portal.js`
and the other fixed files, which `generate-portal-page.py` goes on embedding
unchanged. Its
`--check` rebuilds and compares. It needs Node, so it runs in the `harnesses`
job (the `node:24-trixie-slim` image pinned by digest), not in `checkers`, which
is Python only; it is listed in CONTRIBUTING.md's generators table.

**Reproducible three ways**, in CI on every pipeline that changes `WebUi/`:
1. the `harnesses` image at the checkout path;
2. the same image from another path, with umask 0002, a time zone fourteen
   hours east, a fresh npm cache and later, as `build/ci/linux-repro.sh` varies
   the server's build;
3. `node:24-alpine` (musl), pinned by digest.

All three must give the committed bytes. Measured on the probe: the esbuild
Preact build gave `8c31122cbb7cdba4` at two paths, on glibc and musl, on Node 24
and 20, offline from the cache; the security evaluation also built it on
Windows with win32 binaries and got the same bytes.

**SBOM.** `npm sbom --sbom-format cyclonedx --omit dev` from the lock (built
into npm; CycloneDX 1.5 with hashes and licences), merged into the documents
`build/ci/linux-sbom.sh` makes; Syft in the CI image reads the same lock offline.
The runtime component that ships is `preact` alone; `build/asset-sbom.py` and
`check-sbom-inputs.py` learn it. **Not `@cyclonedx/cyclonedx-npm`**: its
optional `libxmljs2` reached for GitHub (refused by the block) and then fell
back to compiling against headers from nodejs.org.

**Renovate.** The `npm` manager is already enabled and grouped weekly
(renovate.json). A rule for `hmailserver/source/WebUi/**` adds:
`rangeStrategy: pin`; a monthly `lockFileMaintenance`; `minimumReleaseAge` of 14
days for these packages (the repository's 3 days elsewhere); dashboard approval
for a major of `preact`, `esbuild` or `typescript`; and a `prBodyNotes` line:
"rebuild the committed bundle on this branch (`build/generate-webui.py`); the
check fails until you do". `fetchChangeLogs` stays off (the changelogs are on
GitHub).

**Nothing from GitHub.** Every job that runs `npm ci` takes `.no-github` first.
All three evaluations measured every install and build of the chosen toolchain
asking DNS for `registry.npmjs.org` and nothing else (and
`tuf-repo-cdn.sigstore.dev` for the signature check).

**Air-gapped.** The npm cache that a lock fills is archived as a build input in
the project's package registry, as the Windows build's inputs are
(`build-inputs-1`), named by the lock file's SHA-256: about 15 MB for the
production lock on one platform (14,992 KiB measured for `npm ci` into a fresh
cache; the TypeScript compiler's native package is 9.7 MB of it, and the
`--omit=dev` install the bundle is built from fills 5,148 KiB). The archive
holds linux-x64, which serves glibc and musl alike; a win32 build needs its own.
`npm ci --offline` from it reproduced every
probe byte for byte. Distribution source packages never run npm: they ship the
committed output, which is what makes a minified bundle unacceptable there.
(Debian 13's own esbuild 0.25.5 rebuilt the framework-free probe identically but
not the Preact one, because the minifier changed between versions; the
reproducibility claim belongs to the pinned toolchain.)

**Tests.** A second lock of six packages with no install scripts:
`@playwright/test`, `@axe-core/playwright` and `axe-core`, driving Debian's
`chromium` and `firefox-esr` from apt with `PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1`
(Playwright's own browser download comes from Microsoft's CDN and Google Cloud
Storage with TLS-only integrity, CVE-2025-59288). Page and component tests run
in those browsers under the headers the server sends, copied from the C++ as
`check-portal-script.py` already reads them: that is the only layer that sees
CSP and Trusted Types, because neither happy-dom nor jsdom implements Trusted
Types (measured: both accepted a raw `innerHTML` under `trusted-types 'none'`).
Pure modules are tested with Node's built-in `node:test`. axe runs WCAG 2.2 A
and AA on every view in both browsers. vitest with an emulated DOM is not
adopted (64-90 packages, and blind to the property that matters). If Playwright
cannot drive Debian's Firefox, `puppeteer-core` over WebDriver BiDi does (it was
used for the evaluation's Firefox matrix). Measured in this decision's review:
Playwright 1.64 drives Debian's Chromium, and drives Firefox ESR 153.4 only
through its `moz-firefox` channel - its default Firefox launcher fails on it -
so the job names that channel.

**Licences.** What ships is MIT (Preact). The build tree adds Apache-2.0
(TypeScript), all compatible with AGPL-3.0-or-later. Preact's bundle carries no
licence comment, so the generator writes its notice at the head of each bundle
and `docs/Licenses` gains it.

How the pages are served
------------------------

**Until the end of A2 and A3, nothing in the C++ changes.** The Deck stays one
file with one inline script allowed by its hash; the webmail keeps
`script-src 'self'`, the same file names and routes, and `generate-portal-page.py`
keeps embedding the same fixed files. That is what lets the migration move one
view at a time.

**One bundle a page, no code splitting.** The listener speaks HTTP/1.1 only, so
every extra file is another request in turn; the Deck's hash policy allows one
inline script; the webmail's routes are written one file at a time. The OpenPGP
module stays loaded on demand.

**The Deck compiled in** (end of A2). `generate-webui.py` writes
`DeckPageData.cpp`, and `HandleWebAdminPage_` serves the embedded page; the
packages stop installing `WebAdmin/index.html`, and the catalogues are embedded
as the webmail's are. The hash is then fixed at build time and a changed file on
disk changes nothing.

**Cached properly** (A3). Content-hashed file names served by **one generic
route** over a table the generator writes, in place of one hand-written route a
file, with `Cache-Control: public, max-age=31536000, immutable`; the pages
themselves stay `no-store`. That route's table is named in `CompressibleRoute_`
(RestApiServer.cpp:1166-1206), the allow-list of what the server compresses,
which names each path exactly and leaves "a route added later ... plain until it
is named" (its files are the server's own documents, the same bytes for every
reader, which is what the list admits); `pageCache_`, sized at 96 compressed
bodies (:1215), is sized again for the hashed set; and A3's acceptance includes
the webmail shell's size on the wire, which must not grow. Brotli or
precompressed bodies are a separate server change. Today every load pays the
whole shell (about 290 KB gzipped for the webmail) because every script is
`no-store` and the worker is network-first.

**The service worker** stays hand-written (Workbox would add 37 packages with no
npm provenance). It becomes cache-first for the hashed shell, with the cache
named from the build hash so an update replaces the whole shell at once, and a
precache list the build writes, which also holds `/portal-openpgp.js` and the
reader's catalogue (today neither is cached, so by reading of the code an
OpenPGP message cannot be decrypted offline unless the module was loaded earlier
in the same page session). It is registered with `scope: '/portal'` (today its
scope is the whole origin, the Deck included; WebThreatModel.md, W-5) - and
since a registration is kept by scope, today's registration of `/` is taken
away explicitly: the page unregisters `getRegistration('/')` when its scope is
the root, and the worker unregisters itself when `self.registration.scope` is
the root, for at least one release, with a browser test that a page under `/`
is uncontrolled after an upgrade from today's worker.

**Not framework pages, and never to become one:** the seven script-free pages,
from six builders (`/files/{token}`, `/confidential/{token}`,
`/recovery/verify` and `/portal/reset`, which share one, the list pages, the
quarantine digest's links, the migration sign-in result) get the rebuilt
pages' colour, focus and type tokens and **stay script-free C++**, the strictest
policy a page can have; the message-HTML frame stays as it is; the help stays
JSON drawn by the shared Markdown module.

The migration plan
------------------

### A2: the Control Deck, at parity

**Phase 0: the toolchain, with behaviour unchanged.**
- `WebUi/` with the production lock, `.npmrc`, the lock-file and raw-HTML
  checkers, `generate-webui.py` and its `--check`, the three-way reproducibility
  job, the SBOM and the Renovate rule, all under `.no-github`.
- The Deck's inline script split into TypeScript modules and bundled to an IIFE
  whose behaviour is today's: the harness's ~1,210 checks pass unchanged.
- **The Trusted Types block stays as written.** `check-deck-script.py` finds the
  Deck's policy by its comment heading (`/* ---------- Trusted Types ----------`)
  and the exact text `const NEVER=/.../i;`, and requires it before the first
  `.innerHTML`; `trusted-types-test.js` finds it by the same heading. esbuild
  keeps no ordinary comment and writes `const NEVER = /.../` (measured on
  server2 with esbuild 0.28.2: bundled as a module, the heading is gone and
  `check-deck-script.py` reports no policy). So `generate-webui.py` writes the
  block verbatim at the head of the inline script through esbuild's `--banner`,
  which is emitted byte for byte (measured: the bundle then begins with the
  block, and `check-deck-script.py`'s check passes), and both checkers are
  adapted with `check-deck-parity.py` and `check-deck-languages.py` below.
- The harnesses' small DOMs gain what Preact needs before the first view
  moves. On 11.0, five members in the Deck's - `createElementNS`,
  `insertBefore`, `nextSibling`, `remove()`, `style.setProperty` - about 20
  lines, and four in the webmail's. **On 10.29, the on-event properties too**
  (`onclick`, `oninput` and the rest, as `null` on each element): Preact 10 takes
  an event's name from whether `onclick` is a property of the element
  (`lowerCaseName in dom`, src/diff/props.js:84 in 10.29.8), and on an element
  without one it listens for `Click`, so the harness's click never arrives.
  Measured on server2 with the engineering evaluation's stub-DOM probe over
  master's `deck-script-test.js`: 10.29.8 with the five fails the click step,
  and with the on-event properties added passes all eight; `namespaceURI` is
  read but not needed; 11.0.1 passes all eight with the five.
- **`check-deck-parity.py` adapted.** It reads `index.html`'s idioms today:
  `data-view` names, `render*` functions, `data-act` buttons and `api(...)`
  calls. The rebuild keeps one textual idiom the census can read - the router's
  view table in one file, and every REST call written as
  `api('METHOD', '/api/v1/...')` through the one client - and the script gains a
  `--sources hmailserver/source/WebUi/src/deck` mode that reads them.
  **Acceptance:** run over the Phase 0 sources, it gives the census it gives over
  today's `index.html` - 517 written, 507 writable, 507 reached, 10 missing -
  and Phase 1 does not start until it does. `check-deck-languages.py` likewise
  finds every `t('…')` key in the modules, and `check-deck-script.py` and
  `trusted-types-test.js` read the policy where the banner writes it.
- The real-browser job starts with one test a page: it loads under its served
  headers with no violation and no page error, in Chromium and Firefox.
- The A2 spike, inside Phase 0: port one large view (the account editor, about
  630 lines today) and run the full policy matrix on it, on Preact 10.29 and on
  11.0; measure whether `style-src 'unsafe-inline'` can go.

**Phase 1: the views, one at a time, behind the Deck's router.**
- `render(<View/>, el)` mounts a view into `#content`. A container is handed
  back to legacy code only after `render(null, el)`, so a view's effects and
  timers never outlive it; each hand-over has a test.
- Each move deletes that view's `escHtml` and `innerHTML` and carries its
  harness checks into component tests in the real browsers.
- The Roadmap2 row 12 forms (plan decision D7) are built here first.
- The views use CSS logical properties and take `lang` and `dir` from the
  catalogue, so that Arabic, Persian and Hebrew can be added to the Deck with no
  layout work.
- `check-deck-parity.py --strict` joins CI (plan row 52.1). axe runs on every
  moved view.

**Phase 2: the Deck's endgame.** The last `innerHTML` view gone, the Deck's
`default` policy is removed and the header becomes `trusted-types 'none'`; the
catalogues' ban on `< > & "` (`check-deck-languages.py`, `sanitiseCatalogue`)
is replaced by a lint rule that no view inserts a translation as markup; the Deck
is compiled into the binary.

### A3: the webmail and portal, and every other page the server serves

- Island by island: message list, reader, compose, settings, calendar,
  contacts, help. The `id`s the legacy code reads move with each island, and the
  light DOM keeps `aria-activedescendant` and `aria-labelledby` working.
- The NVDA-pass behaviours (`portal-script-test.js:8494-8987`) are the
  acceptance tests of the list and the dialogs; an NVDA pass of the rebuilt
  pages in the three right-to-left languages and one other closes it.
- The protected-message page moves to the same toolchain.
- The one named Trusted Types policy replaces `default`; the hashed files, the
  generic route (named in `CompressibleRoute_`) and the cache-first worker with
  its narrowed scope, today's root registration taken away.
- The seven script-free pages restyled to the shared tokens and left script-free.

### A4: verification that keeps running

- **A ZAP job.** `zaproxy/zap-stable` from Docker Hub, pinned by digest, with
  add-on updates off (ZAP fetches add-ons from GitHub; the job runs under
  `.no-github` and fails if it tries). Against a server the same pipeline built,
  started in a container: a baseline scan of the anonymous pages; an API scan
  driven by `/api/v1/openapi.json` with a full API key, and again with a Deck
  session (a replacer rule adds `X-Requested-With: hMailServer`). New alerts of
  medium or above fail the job; known ones live in a baseline file with a reason
  each, as `semgrep-baseline.txt` does.
- **Fuzzing,** in the existing `fuzz` schedule (`linux-fuzz`): libFuzzer
  targets for the HTTP request framing, for `ParseRoute_` with
  `DecodePathSegment_` (reached through a tester, as `RestApiOpenApiTester` is),
  and for the JSON document parser; for the page modules, a seeded
  property-based loop in `node:test` over the Markdown renderer, the catalogue
  functions and the Trusted Types policy (no new package).
- **A CSP report endpoint.** `POST /api/v1/csp-report`, anonymous because
  browsers send reports without credentials, answered before `Authenticate_` like
  the other anonymous routes: a body cap of 8 KB, the budget per address the
  protected-message pages use, `application/reports+json` and
  `application/csp-report` only, nothing echoed, nothing written to the
  database; counted in memory, one log line per distinct violation a window,
  and shown in the Deck's diagnostics. The pages name it with
  `Reporting-Endpoints` and `report-to`, and with `report-uri` for browsers that
  have only that. It goes into the authorisation matrix like every other route.
- **The outbound requests.** For each address a sender or an account chooses
  (WebThreatModel.md, Entry points, "Outbound requests": the one-click
  unsubscribe URL, Web Push endpoints, Web Key Directory lookups, a reader's
  fetch host) and for an inbound webhook made by a confined credential, a test
  that a private, loopback, link-local and metadata address is refused, through
  the proxy and without it.
- **The policy endgame for the webmail.**

### A5: the independent review

Commissioned by the owner (plan O14), on the code at the end of A4.

Landing
-------

Row A1 - this decision, the threat model, and the authorisation matrix with its
generator in the `checkers` job - is in 6.3.7. The rebuild is 6.4's: four
batches, each a branch folded and landed by the standing rules (the full
Windows and Linux gates, the pre-gate of every blocking job on the final tip,
`build/land-via-mr.py`):

| Batch | Contents |
|---|---|
| **PA-1** | Phase 0 of A2 - the toolchain, the checkers, the reproducibility, SBOM and Renovate wiring, the module split with behaviour unchanged, the harness DOM members, the parity census reading the sources, and the first real-browser job. No behaviour changes. |
| **PA-2** | A2 Phases 1 and 2: the Deck's views, the R2-12 forms, `check-deck-parity.py --strict`, axe on every view, the Deck's policy endgame, and the Deck compiled into the binary. |
| **PA-3** | A3: the webmail, portal and protected page, the named policy, the hashed files with the generic route, the worker with its narrowed scope, and the script-free pages restyled. |
| **PA-4** | A4: the ZAP job, the fuzz targets, the CSP report endpoint, and the webmail's policy endgame. |

The matrix is regenerated by every change of a route and held to the code by
its `--check` in the `checkers` job, so each batch of the rebuild starts from the
authorisation as the code decides it then; the threat model is revised at the
end of each batch (WebThreatModel.md, "How this stays true").

What would change this decision
-------------------------------

- **The A2 spike finds Preact 10.29 and 11 both failing a policy cell or the
  harness:** no framework, same toolchain and phases.
- **A React-only library becomes necessary** (an accessible component library,
  say): React 19, after weighing that library's own tree under rule 4.
- **Preact's provenance stops, or its maintenance stalls** (several releases with
  no response): React or no framework, view by view; the templates move by an
  import change or a typed `h()`.
- **Lit routes `unsafeHTML` to a bare sink, or Vue `v-html`:** either would come
  within reach on the security lens; re-run the matrix.
- **The owner rules out every third-party runtime:** no framework.
- **A distribution must build the front end from its own archive:** no
  framework, which Debian's own esbuild rebuilds identically.

Evidence
--------

The three evaluations, their lab scripts, lock files and raw browser results,
and the decider's re-check are kept with Programme A's working files outside
the repository (row A1). Every number above is from them; each can
be run again in about a minute a candidate on the project's Linux builder.
