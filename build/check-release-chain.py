#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The release jobs of .gitlab-ci.yml are what a server built after 6.3.3 needs them to be.

A v* tag's pipeline ends in the release chain (docs/ContinuousIntegration.md, "The
release"): windows-installer, release, release-authenticode, release-sbom,
release-sign, release-verify and release-publish, then pages and package-repos, which
deploy the update feed with the release in it. Most of what makes a release
installable is fixed in places that do not look like each other - the identity
SigstoreVerifier.cpp compiles in, the file the pipeline is defined in, the ID
token's audience, cosign's flags, the order of the jobs - and a change to any one
of them is a release no server installs, found only when the first server
refuses it. This checker holds them to each other. It fails when:

  * release-sign is not a job of this file with an ID token for Sigstore (aud
    sigstore), is a trigger (a child or downstream pipeline's token carries no
    ci_config_ref_uri, and Fulcio issues it no certificate), or runs in an image
    that is not cosign's own from gcr.io pinned by digest (a tag with the
    placeholder PIN-THE-DIGEST in place of its digest included);
  * build/ci/release-sign.sh stops naming the bundle format and the log a server
    reads (--new-bundle-format, Rekor v1 at rekor.sigstore.dev, the signing
    configuration off, hashedrekord 0.0.1), or counts another number of files than
    a release carries (build/ci/release-chain.py);
  * the identity the scripts expect is not the one SigstoreVerifier.cpp trusts
    at the group's path the project has been at since 29 September 2026
    (GROUP_PATH), or that one is not this project's path, file and repository
    id; or the verifier stops trusting it once, with the project's id required,
    or stops trusting the path the project began at (FIRST_PATH), which signed
    6.3.5, once, with that id required, or stops requiring the id of every
    certificate naming either path or repository whatever the settings; or the
    scripts sign from any path but the group's, or name the identity twice;
  * a compiled file the release carries (build/ci/release-chain.py's
    expected_assets: an installer or a package) goes out without its own SBOM,
    <file>.cdx.json, beside it (OpenSSF Baseline OSPS-QA-02.02); release-sbom
    does not make the installer's from the bytes Authenticode signed, before
    release-sign signs it; release-sign.sh does not take that SBOM and hold it
    to the signed installer; or release-verify.sh does not hold every compiled
    file's SBOM to the file with build/asset-sbom.py verify;
  * release-authenticode has no ID token for Entra ID (aud
    api://AzureADTokenExchange), or its script signs without the service's RFC
    3161 timestamp, or reads other settings than the release job checks;
  * the ARM64 installer (hMailServer-<v>-arm64.exe, from 6.3.6) is not built,
    signed and described as the x64 one is: release must need
    windows-packages-arm64 (.gitlab/ci/packages-windows.yml) with its artifacts, not
    optionally, and that job may not be allow_failure or manual in a v* tag's
    pipeline; release-chain.py must collect it from packages-windows/arm64/ held to
    that job's SHA256SUMS; release-authenticode must sign it into
    release-authenticode-arm64/, the folder release-chain.py's sbom and
    release-sign.sh read it from, and keep that folder; release (and the rehearsal)
    must keep linux-sbom's ARM64 component list for its SBOM; release-sign.sh must
    take it signed with its SBOM; and the rehearsal must need
    windows-packages-arm64 too;
  * windows-packages-x64 is in a v* tag's pipeline, whose x64 installer is
    windows-installer's alone, or windows-packages-arm64 needs it without
    optional: true, which would keep a tag's pipeline from being created;
  * a job or template of the CI files that runs on bench150 does not take
    hooks: !reference [.bench150-leftovers, hooks], the step that stops before
    its checkout what a cancelled job left running in its directory, or
    .bench150-leftovers has no pre_get_sources_script;
  * the PowerShell module's HmailServer.psd1 and HmailServer.psm1 are not files every
    release carries, collected unsigned from hmailserver/source/Tools/Cli and signed
    after collect, without an SBOM; release-authenticode does not sign them by a call
    of their own, made only when the ARM64 installer's signed, into
    release-authenticode-module/, which it keeps and release-chain.py's sbom and
    release-sign.sh read; or windows-authenticode.ps1 stops holding each signed file
    to its unsigned bytes followed by one signature block;
  * the job build-inputs-authenticode (.gitlab/ci/packages-windows.yml), which holds
    the build inputs to the Authenticode signatures third-party-binaries.json records,
    is missing, runs elsewhere than bench150 or outside .own-runners' rules, goes on to
    its self-test after the check failed, or can let a job that needs it start without
    its having passed; or the Windows package jobs or windows-installer do not need it;
  * the release or release-publish job can start without a person (when: manual,
    allow_failure: false), a release job can run for anything but a protected v*
    tag, or the jobs' needs no longer put Authenticode before Sigstore, Sigstore
    before the verifier, and the verifier before the upload;
  * release does not wait for every job of .gitlab-ci.yml that can fail a tag's
    pipeline (every job that is neither allow_failure nor manual, nor one of the few
    that never runs in one; the package jobs of the included .gitlab/ci/packages-*.yml
    are not release gates but for those whose files a release carries), or brings the
    artifacts of any but the jobs whose files it collects and the four scans whose
    reports it reads;
  * a package a release carries from a package job (CARRIED_PACKAGES: the generic
    arm64 .deb and aarch64 .rpm) is not built, described and installed before release
    starts: the job that builds it, the job that makes its SBOM
    (build/ci/packages/package-sbom.sh, keeping <package>.cdx.json beside it) and the
    job that installs it must be in their file, the two after the build must need it,
    and release must need all three, not optionally, the first two with their
    artifacts; none of the three may be allow_failure or manual in a v* tag's
    pipeline, in the job, a template it extends or a rule a tag can take (a needed
    job that may fail lets release start after it failed); and
    build/ci/release-chain.py must collect the file and its SBOM from where that
    build leaves them, as a compiled file;
  * the release job's tag check (build/ci/release-chain.py collect), given a git
    that answers as the job's checkout would, accepts a tag signed by a key the
    tagged tree's own .gitlab/allowed_signers lists and master's does not, a tag
    on a commit that is not on master, or a tag while master's list holds a
    second key, or refuses a tag master's one key signed on master's history; or
    the tree's .gitlab/allowed_signers lists more than one key, which the release
    job refuses as master's (release tags are the owner's alone, GOVERNANCE.md);
  * collect, given a registry that holds files of the release's version, takes
    one with other bytes, or the installer, a bundle, a bundle's .sigstore.json copy
    or the provenance, which only a signing after it makes, or refuses one with the
    same bytes; or publish uploads before
    it has refused a name the registry holds with other bytes, sends again what
    is up already, or holds only the newest record of a name to the digest;
  * the pinned-image pattern the release job holds HM_COSIGN_IMAGE to at run time
    (build/ci/release-chain.py) is not this checker's, which a CI/CD variable of
    the same name could otherwise take past;
  * the feed does not follow the release in the tag's own pipeline: pages must run
    for a protected v* tag and need release-publish - optionally, since that job is
    in a v* tag's pipeline alone, and without its artifacts - and nothing else, and
    package-repos, which deploys the site, must need pages with its artifacts; or a
    job of the CI files triggers a pipeline of another project or of this one
    (trigger: project), which GitLab drops as invalid_bridge_trigger when the project
    is its own, as it dropped 6.3.5's update-feed, and which would name a path the
    project's move into the group changes. Every job but pages and the few whose own
    rules can only match something else, each named here with why, refuses a
    schedule's pipeline and a downstream one (source "pipeline"), which nothing here
    starts;
  * a scheduled pipeline is not chosen by its schedule's description: linux-fuzz,
    linux-coverage, renovate, forge-export (.gitlab/ci/forge-export.yml) and secret_detection
    (.gitlab/ci/gitlab-security.yml, the whole history) each run for the schedule
    described "fuzz", "coverage", "renovate", "export" or "security", and pages
    refuses those five, and package-repos (.gitlab/ci/package-repos.yml) takes pages' rules and no
    others; or a rule of those files still names
    HM_PIPELINE or RUN_RENOVATE, the variables that chose them before, which the
    project would refuse on a schedule - one way in;
  * the image pins are not asked of their registries: the image-pins job must run
    build/check-image-pins.py's self-test and the check in the pipeline of the
    schedule described "renovate" on a protected ref, and may not be allow_failure,
    so that a digest a registry no longer serves fails that pipeline, daily, and not
    first the job that pulls it;
  * the jobs on the project's own runners do not skip a push that changes
    documentation alone, or skip one that changes anything else: the rules:changes
    paths of .own-runners (which db-matrix shares) are matched against every file
    of the tree as GitLab matches them, and must leave out exactly the Markdown
    files at the root and under .gitlab/ - hmailserver/docs/*.md is compiled into
    the server;
  * linux-suite or windows-gate can be cancelled by a later push (interruptible),
    or a gate that lets a busy bench's exit 10 pass (allow_failure: exit_codes)
    can do so in a v* tag's pipeline, where release would then start without it;
  * linux-fuzz does not keep fuzz-results/ as its artifact, whatever the result,
    for Developers only (a reproducer may be an exploit), or another job claims it;
  * secret_detection lets a finding pass: it must be allow_failure: false and read
    its report after the analyzer, which exits 0 whatever it finds, and put the
    rules build/ci/secret-detection-rules.sh derives from GitLab's in place of
    /gitleaks.toml before it, since GitLab's own leave out every path under a
    directory named build, bin, obj, target or packages; or runs another
    image than the analyzer as the project's registry holds it, on the one line
    build/scan-secrets.py reads (image: mirror/secrets:<version>-<the digest's first
    twelve hex digits>@<digest>, build/ci/mirror-secrets.sh);
  * the release can be collected past the security scans: release must need
    static-analysis, semgrep, dependency-scan and GitLab's semgrep-sast, not
    optionally, with their artifacts - the scans build/ci/security-gate.py reads -
    semgrep-sast must run for a protected v* tag and keep gl-sast-report.json as a
    file whatever it found, and collect must ask the gate before it copies anything,
    refusing a tagged tree that commits a scan's report and a semgrep-sast report
    older than the tag's pipeline;
    or static-analysis or semgrep, which fail on a finding new to their baselines,
    is allow_failure, so that a new finding would pass a branch's pipeline unseen;
    or .gitlab/security-exceptions.yml is not a file the gate reads, or accepts a
    finding without a reason, a reviewer or an expiry within a year of the review, or
    is not the owner's in .gitlab/CODEOWNERS (nor .gitleaksignore, the secret scan's
    list of what is not a secret).
  * a change can land past the change gates (.gitlab/ci/mr-checks.yml): semgrep-sast-gate
    and mr-dependency-check must each run in a merge request's pipeline and in a
    protected batch* branch's, with no rules:changes list, no when: never, no
    SAST_DISABLED and no allow_failure, find their base themselves (--merge-request
    auto, --base auto), and be retried twice on their script's exit 2 and on
    .gate-retry's reasons; semgrep-sast must run on a protected batch* branch too, read
    no SAST_DISABLED, take .gate-retry, and run the analyzer as the project's registry
    holds it (mirror/semgrep:<version>-<the digest's first twelve hex digits>@<digest>),
    not allow_failure, removing gl-sast-report.json and semgrep.sarif before the
    analyzer runs and keeping both, every entry of its SAST_EXCLUDED_PATHS held to the
    root, and SAST_SEMGREP_EXCLUDED_PATHS set to nothing in its own job; no CI file may
    set SAST_SEMGREP_EXCLUDED_PATHS, SAST_RULESET_GIT_REFERENCE,
    SAST_EXCLUDED_ANALYZERS or HM_TIDY_CHECKS, or SAST_EXCLUDED_PATHS but semgrep-sast's
    own, or define a variable GitLab gives (CI_*, GITLAB_*, but CI_DEBUG_SERVICES); the
    tree may carry no .semgrepignore, no .gitlab/sast-ruleset.toml, no
    .gitlab/secret-detection-ruleset.toml and no scan's
    report; .gitlab-ci.yml's first workflow rule must leave a merge request without a
    pipeline of its own only when it comes from a protected batch* branch of this
    project, the branches the gates run for in a branch's pipeline;
    static-analysis, semgrep, ci-image-packages and dependency-scan must compare a
    protected batch* branch with master (compare_to: refs/heads/master) and run for
    none that changes documentation alone; and the jobs build/land-via-mr.py requires
    must be jobs of the CI files - the two gates, semgrep-sast and checkers always
    (GATE_JOBS), static-analysis and semgrep (SOURCE_SCAN_JOBS) - and its
    documents_only, which lets the second two off, must agree with .own-runners'
    paths: *not-docs-only on every file of the tree; its GATE_FILES, which a batch
    changes only with --gates-read, must name the gates' code, .gitlab-ci.yml, every CI
    file the gate jobs are in or reach, the two scripts it runs and the two source
    scans' scripts; no CI file but one may define a job that holds a change (the
    gates, semgrep-sast, static-analysis, semgrep, checkers) or a template one reaches,
    since GitLab merges a second definition into the first; no CI file but
    .gitlab-ci.yml may set variables, default or workflow, or include anything but
    GitLab's templates; and each Python script of GATE_FILES must keep its own
    directory off sys.path before it imports anything but os and sys (PATH_GUARD).
  * the container image's jobs (.gitlab/ci/container-image.yml) can run for
    anything but a protected v* tag, start before release-publish or without a
    person (container-image-deb, the first, is manual; every other one follows it),
    build from anything but the .deb container-image-deb took from the published
    release and verified against its bundle with cosign - as this project's
    pipeline for the tag, with the project's id at the group's path, and no token
    sent over the TLS busybox's wget does not check - push anywhere but the
    group's path, registry.gitlab.com/hmailserver/hmailserver, sign before the push, sign
    without an ID token for Sigstore or in another image than HM_COSIGN_IMAGE, or
    build/ci/container-image-sign.sh expects another identity or log than the
    release's files are signed with, or no longer attests each platform's SBOM
    (container-image-sbom, build/ci/container-image-sbom.sh) to that platform's image
    and verifies the attestation; or container-image-start does not start the pushed
    image with the command its test script names, on a network of its own; or
    container_scanning does not run build/ci/container-scan.sh over the linux/amd64
    image in dependency-scan's Trivy image, by the same digest (CONTAINER_SCANNER),
    keeping its report, or container_scanning_arm64 does not extend it for the
    linux/arm64 image (HM_SCAN_ARCH: arm64) or has an image, a script, a
    before_script, artifacts or an allow_failure of its own, or either does not take
    container-image's dotenv report, which names each platform's image by its
    digest: each must have one dependencies key, [container-image], and one needs
    key, whose entry for container-image has one job key and one artifacts key,
    true, and take no rule with needs of its own (GitLab hands a job that uses needs
    the reports of the needs with artifacts alone, whatever its dependencies say,
    its YAML reader takes the last of two keys, and a rule's needs replace the
    job's); or
    container-scan.sh no longer takes each platform's image by its digest, the
    project's alone, or a CI file names GitLab's container-scanning analyzer or
    includes its template again (GITLAB_SCANNER) - an image GitLab rebuilds daily
    and stops serving within days of, which may not be copied; or the
    switch, the project's own CI/CD variable HM_PUBLISH_IMAGE, is not read as the jobs
    run - container-image-deb.sh, linux-image.sh and container-image-push.sh must
    refuse, exiting non-zero, unless it is "1" - or is read by a rule, which is
    decided when the pipeline is made and would leave a release tagged before it was
    set without the jobs; or a committed CI file sets that variable;
  * the two container scans are written in a form this does not read: it reads
    their needs, dependencies and rules as this tree writes its CI files - a lint of
    them against drift, not a YAML parser - and refuses each other form of them that
    GitLab's YAML reader takes, rather than read past it: a key of either job, of
    its entry for container-image or of a template whose rules it takes that is not
    bare (quoted, a space before its colon, a merge key), a key of the job written
    twice, or a second job key in that entry; in the rules it takes and each block
    their !reference [X, rules] names, rules: with anything after it on its line or
    written twice, a list not indented four, or a rule that is not a list item of
    bare keys or !reference [X, rules], X a block of .gitlab-ci.yml or
    container-image.yml; either job, or a template it reaches, defined twice in the
    CI files; and a line at a CI file's top level that is not a key. GitLab's own
    templates are not read, nor what GitLab refuses itself when the pipeline is made
    (a need from another pipeline beside dependencies, an alias without its anchor);
  * a platform of the image (IMAGE_PLATFORMS, linux/amd64 and linux/arm64) is left
    out anywhere: the Dockerfile must take the .deb of TARGETARCH and run the server
    in the build, linux-image.sh build for the platform asked for, a branch's
    pipeline build each platform (linux-image, linux-image-arm64 from
    deb-ubuntu26.04-arm64's .deb, both needed by release), a tag's pipeline take each
    architecture's .deb and SBOM from the release, build each platform's image in a
    job of its own, push both under one index with regctl pinned by digest (the -alpine
    variant of the release HM_REGCTL_IMAGE pins), make, sign and attest each
    platform's SBOM, sign the index, scan each platform's image and start each, the
    arm64 one as a service of that platform; and the chart's artifacthub.io/images
    annotation, and helm-chart-package.sh, which holds the packaged chart to it, must
    list exactly those platforms;
  * Artifact Hub's metadata for the image's repository can be pushed but by
    container-image-artifacthub (.gitlab/ci/container-image.yml): it must take
    .release-rules and, beside them, only a manual rule for the protected default
    branch that may fail; need container-image optionally and without its artifacts,
    and nothing may need it; run in container-image's regctl image; and
    build/ci/container-image-artifacthub.sh must refuse unless HM_PUBLISH_IMAGE is 1,
    push to the group's path alone, under the tag artifacthub.io alone - regctl run for
    nothing but its login, its logout, reads and that one artifact put - the tree's
    packaging/docker/artifacthub-repo.yml as the one layer of the media type Artifact
    Hub reads, with the empty configuration of its type, and read it back byte for
    byte; and that file may hold nothing but comments, blank lines and one
    "repositoryID: <a UUID>";
  * container-image-push.sh can move :latest or :<major>.<minor> backwards: it must
    list the repository's tags, make a tag that does not exist, read the version label
    of the image an existing one names, move it for this version or an older one and
    leave it for a newer one or one it cannot read, put no tag on the index but through
    move(), and its version_order, run here under sh, must compare number by number
    (6.3.10 is newer than 6.3.9);
  * a file this checker reads is not in the tree (reported, and never a traceback);
  * a file the release carries from 6.3.6 is not gathered, described and tested
    before release starts: each distribution release's .deb and .rpm, the two
    AppImages and the source tarball (build/ci/release-chain.py's gathered_builds)
    must each be made by a job of the package files, and the release-packages job
    (.gitlab/ci/release-packages.yml) must need each build with its artifacts and
    each install or AppImage test without, none optionally, run
    build/ci/release-chain.py gather and keep release-packages/, and run for a v* tag
    in the pipelines .own-runners makes the package jobs in; none of those jobs, nor
    release-packages, may be allow_failure or manual in a v* tag's pipeline, nor wait
    for a press on a batch* branch, where the release is rehearsed; release must need
    release-packages and helm-chart-package with their artifacts, not optionally; or a
    job needs more than GitLab's fifty (ci_needs_size_limit on GitLab.com), which
    would stop the pipeline from being created at all;
  * the release carries the AppImages without the source of the libraries linked
    into their runtime (hmailserver-<v>-appimage-runtime-sources.tar.gz, which the
    LGPL-2.1 of libfuse3 and the GPL of lzo ask to go with them): release-chain.py
    must gather it from the appimage-runtime-sources job's artifact, held to that
    job's sums, as a primary file without an SBOM, and that job must make it with
    build/ci/packages/appimage-runtime-sources.sh from both runtime jobs, whose
    licence files each AppImage job needs; or without the source of the libraries they
    carry in usr/lib (hmailserver-<v>-appimage-bundled-sources.tar.gz, which the LGPL
    of GnuTLS and the others asks to be offered with them): release-chain.py must
    gather it from the appimage-bundled-sources job's artifact, held to that job's
    sums, as a primary file without an SBOM, that job must make it with
    build/ci/packages/appimage-bundled-sources.sh from both AppImage jobs' records, and
    build-appimage.sh, appimage-test.sh and that script must name it as release-chain.py
    does, and the image carry hMailServer's own AGPL-3.0 text; or either sources job
    does not keep GitHub out of itself with .no-github (.gitlab/ci/no-github.yml),
    whose before_script must be the one the job runs - taken in its extends where
    nothing nearer has a before_script, or as the first line of the one that decides
    it - since both fetch sources by recipes some of which name GitHub; or a Linux file
    the release carries, the chart or either sources file has no line in the release's
    sums;
  * release-sign.sh stops copying each primary file's bundle to <file>.sigstore.json,
    attesting the provenance's statement (cosign attest-blob --statement, in the
    bundle format, to Rekor v1) or finding every file in it with cosign
    verify-blob-attestation as the pipeline's identity, or counts another number;
    release-verify.sh stops holding every compiled file and the tarball to its SBOM,
    or the copies and the provenance to the files (release-chain.py
    check-provenance); or release-chain.py's publish uploads before it has held the
    copies, the provenance, GitLab's rules for the links and the site's budget
    (_redirects, the feed's size) to the release, or collect copies before counting
    that budget; or its limit on the feed's size is not UpdateChecker.cpp's;
  * the rehearsal (.gitlab/ci/release-rehearsal.yml) can run for a tag, for master,
    for a merge request or where the package jobs are not made; runs anything but
    release-chain.py collect --rehearse and sbom --rehearse, release-sign.sh
    --rehearse and release-verify.sh --rehearse; holds an ID token but Sigstore's in
    its signing job, or names the job token, the registry or publish; or the scripts
    sign as a branch other than in their rehearsal, or as one that is not a protected
    batch* branch, or release-chain.py takes --rehearse with publish or gather;
  * a Fulcio certificate's field is read by searching the certificate's bytes, which
    its public key - bytes its holder chooses - can fill with whatever is searched
    for: release-verify.sh must read the installer's repository id
    (1.3.6.1.4.1.57264.1.15) with build/ci/sigstore-certificate.py, and
    container-image-deb.sh, in cosign's image without Python, with its shell twin
    build/ci/sigstore-certificate.sh; both readers must answer every case of
    sigstore-certificate.py's own (this project's id planted in a key in front of the
    extension naming another, 6.3.3's real Fulcio certificate so planted, the
    extension twice or holding anything but one UTF8String, a certificate cut short,
    a length DER would write shorter, and bundles whose chain's first certificate -
    the one cosign verifies - is spelled so that a count of "rawBytes" misses it,
    its second carrying this project's id) as the cases say, the shell one under sh here
    (a CI job without sh fails); and no file of build/ or .gitlab/ but those two may
    hold the Sigstore OID arc 1.3.6.1.4.1.57264 as bytes - as hex in any spelling,
    as decimal numbers, or as the base64 of a certificate that holds it; and
    container-image-deb.sh must refuse a cosign without verify-blob
    --new-bundle-format, which alone reads the bundle strictly enough for the shell
    reader's one certificate to be the one cosign verified.
  * a job of the CI files can run on a runner that is not the project's in a pipeline
    for a protected ref that is not a merge request's, or wait for a runner that never
    takes it in any other (runner_problems; docs/ContinuousIntegration.md, "Every job
    on the project's own runners"): .gitlab-ci.yml's default: must name the tags
    [$HM_RUNNER], which every job that names none of its own takes - GitLab's
    templates' jobs among them - and its variables: must set HM_RUNNER to linix, which
    nothing else in the CI files may set but a workflow rule, and no job may say
    inherit:; the workflow's rules, tried for every kind of pipeline and every way their
    other clauses can go, must leave it linix for each pipeline they make of a
    protected ref that is not a merge request's and set it to saas-linux-small-amd64,
    GitLab's hosted runner, for each other one, which the project's runners -
    registered ref_protected - take nothing of; and a job that names its own tags must
    name a runner of the project's (every tag one runner's, one of them linix,
    linix-systemd or bench150: not none, not GitLab's, not another variable), and then
    its rules may make it in no pipeline of a merge request or an unprotected ref.
    A class of the jobs that need nothing of the project's takes its tags from a switch
    instead (RUNNER_CLASSES; docs/ContinuousIntegration.md, "GitLab's hosted runners
    beside the project's"): HM_RUNNER_BUILD, HM_RUNNER_PACKAGES, HM_RUNNER_ARM64 and
    HM_RUNNER_SUITE, which variables: must set to linix and nothing else in the CI files
    may set - the owner's protected project variable of the same name moves the class -
    and a class with a lane takes its resource group from HM_LANE_PACKAGES or
    HM_LANE_SUITE, set there to linix-package-build and linix-suite. A job takes a
    switch only when its class names it, every job a class names takes it, and its
    rules are held as a job's on linix are; a classed job names no environment,
    id_tokens or secrets, asks for a pull policy GitLab's runners allow (always, after
    if-not-present), runs in a container of its class's architecture (the CI image is
    amd64 alone) and in its class's lane or in none; no other job waits in a lane; nix
    asks for the tags linix and kvm, which server2 alone carries; and
    .gitlab/ci/hosted-runners.yml, which .gitlab-ci.yml includes, runs
    build/ci/hosted-runners.py's check and quota on linix in a pipeline any switch or
    lane is set for, compute-quota in the environment compute-quota and hosted-runners
    in none, with the script's classes the CI files' (runner_class_problems).

Usage:
  python build/check-release-chain.py              the tree as it is
  python build/check-release-chain.py --self-test  each rule broken in a copy, and caught
"""

import os
import sys

# Nothing is imported by name from this script's own directory, which Python puts first
# on sys.path: a file put there named as one of Python's modules (json.py, re.py) would
# run as this script's code in a change that leaves the script alone
# (build/check-release-chain.py, PATH_GUARD).
sys.path[:] = [p for p in sys.path if os.path.normcase(os.path.realpath(p or os.curdir)) !=
               os.path.normcase(os.path.dirname(os.path.realpath(__file__)))]

import ast
import base64
import importlib.util
import io
import json
import re
import subprocess
import shutil
import tempfile
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = {
    "ci": ".gitlab-ci.yml",
    "security": os.path.join(".gitlab", "ci", "gitlab-security.yml"),
    "sign": os.path.join("build", "ci", "release-sign.sh"),
    "verify": os.path.join("build", "ci", "release-verify.sh"),
    "authenticode": os.path.join("build", "ci", "windows-authenticode.ps1"),
    "installer": os.path.join("build", "ci", "windows-installer.ps1"),
    "verifier": os.path.join("hmailserver", "source", "Server", "Common", "Util", "SigstoreVerifier.cpp"),
    "fuzz": os.path.join("build", "ci", "linux-fuzz.sh"),
    "chain": os.path.join("build", "ci", "release-chain.py"),
    "signers": os.path.join(".gitlab", "allowed_signers"),
    "image": os.path.join(".gitlab", "ci", "container-image.yml"),
    "image-sign": os.path.join("build", "ci", "container-image-sign.sh"),
    "image-deb": os.path.join("build", "ci", "container-image-deb.sh"),
    "image-build": os.path.join("build", "ci", "linux-image.sh"),
    "image-push": os.path.join("build", "ci", "container-image-push.sh"),
    "image-sbom": os.path.join("build", "ci", "container-image-sbom.sh"),
    "dockerfile": os.path.join("hmailserver", "source", "Server", "platform", "packaging", "docker", "Dockerfile"),
    "image-start": os.path.join("build", "ci", "container-image-start-test.sh"),
    "image-scan": os.path.join("build", "ci", "container-scan.sh"),
    # Artifact Hub: the image's repository's metadata and its push; the chart, whose
    # images annotation names the image's platforms, and its packaging.
    "image-artifacthub": os.path.join("build", "ci", "container-image-artifacthub.sh"),
    "image-artifacthub-file": os.path.join("hmailserver", "source", "Server", "platform", "packaging", "docker", "artifacthub-repo.yml"),
    "chart": os.path.join("hmailserver", "source", "Server", "platform", "packaging", "helm", "hmailserver", "Chart.yaml"),
    "chart-package": os.path.join("build", "ci", "helm-chart-package.sh"),
    "export": os.path.join(".gitlab", "ci", "forge-export.yml"),
    # The package builds (.gitlab-ci.yml includes them): held to the old routes too.
    "packages-deb": os.path.join(".gitlab", "ci", "packages-deb.yml"),
    "packages-rpm": os.path.join(".gitlab", "ci", "packages-rpm.yml"),
    "packages-appimage": os.path.join(".gitlab", "ci", "packages-appimage.yml"),
    "packages-windows": os.path.join(".gitlab", "ci", "packages-windows.yml"),
    "repos": os.path.join(".gitlab", "ci", "package-repos.yml"),
    "exceptions": os.path.join(".gitlab", "security-exceptions.yml"),
    "codeowners": os.path.join(".gitlab", "CODEOWNERS"),
    # What a release carries from 6.3.6 besides the generic packages, gathered; the
    # chart; the source tarball; and the release rehearsed on a batch* branch.
    "release-packages": os.path.join(".gitlab", "ci", "release-packages.yml"),
    "helm": os.path.join(".gitlab", "ci", "helm-chart.yml"),
    "tarball-ci": os.path.join(".gitlab", "ci", "source-tarball.yml"),
    "rehearsal": os.path.join(".gitlab", "ci", "release-rehearsal.yml"),
    "checker": os.path.join("hmailserver", "source", "Server", "Common", "Util", "UpdateChecker.cpp"),
    # What names the source of the AppImages' bundled libraries: the script that packs
    # the image and lists them, the test that holds the image to it, and the script that
    # packs the source.
    "appimage-pack": os.path.join("hmailserver", "source", "Server", "platform", "packaging", "build-appimage.sh"),
    "appimage-test": os.path.join("build", "ci", "packages", "appimage-test.sh"),
    "bundled-sources": os.path.join("build", "ci", "packages", "appimage-bundled-sources.sh"),
    # .no-github, which keeps GitHub out of a job itself.
    "no-github": os.path.join(".gitlab", "ci", "no-github.yml"),
    # The two change gates, and the landing script that requires them.
    "mr-checks": os.path.join(".gitlab", "ci", "mr-checks.yml"),
    "land": os.path.join("build", "land-via-mr.py"),
}

RELEASE_JOBS = ["windows-installer", "release", "release-authenticode", "release-sbom", "release-sign", "release-verify",
                "release-publish"]
# The container image's jobs, in .gitlab/ci/container-image.yml, and what each must
# need: the .deb files are taken from the release once it is published, each
# platform's image built from its own and checked, both pushed under one index, and
# signed, scanned and started once they are pushed.
IMAGE_JOBS = {
    "container-image-deb": ["release-publish"],
    "container-image-amd64": ["container-image-deb"],
    "container-image-arm64": ["container-image-deb"],
    "container-image": ["container-image-amd64", "container-image-arm64"],
    "container-image-sbom": ["container-image", "container-image-deb"],
    "container-image-sign": ["container-image", "container-image-sbom", "container-image-start", "container-image-start-arm64"],
    "container_scanning": ["container-image"],
    "container_scanning_arm64": ["container-image"],
    "container-image-start": ["container-image"],
    "container-image-start-arm64": ["container-image"],
}
# The platforms the image is built for, as dpkg and the OCI name their architectures:
# one image of each under one index, each from the release's .deb of its architecture.
IMAGE_PLATFORMS = ("amd64", "arm64")
# The image container-image pushes with: regctl's -alpine variant, from Docker Hub,
# pinned by digest, at the release the channels' HM_REGCTL_IMAGE pins.
REGCTL_ALPINE = re.compile(r"^docker\.io/regclient/regctl:(v[0-9][A-Za-z0-9._\-]*)-alpine@sha256:[0-9a-f]{64}$")
CHANNELS_REGCTL = re.compile(r'^    HM_REGCTL_IMAGE: "docker\.io/regclient/regctl:(v[0-9][A-Za-z0-9._\-]*)@sha256:[0-9a-f]{64}"\s*$', re.MULTILINE)
# The first of them, which a maintainer starts by hand; every other one needs it, and
# so runs only after it.
IMAGE_FIRST = "container-image-deb"
# The switch: the project's own CI/CD variable HM_PUBLISH_IMAGE, read as the jobs RUN.
# The maintainers set it after the project's move into the group hmailserver (made on
# 29 September 2026), which they decided on 27 September 2026 to make before the first
# image, so that the first image's address, registry.gitlab.com/hmailserver/hmailserver,
# is the lasting one (a
# container repository's path is always its project's). The jobs are in every v* tag's
# pipeline, so that a release published before the move has its image pushed from its
# own pipeline afterwards; a rule reading the variable would be decided when the
# pipeline is made, and would leave that release without them. The two scripts that
# fetch and push refuse, exiting non-zero, unless it is "1". Nothing committed may set it.
RUNTIME_GATE = 'if [ "${HM_PUBLISH_IMAGE:-}" != 1 ]; then'
GATED_SCRIPTS = ("image-deb", "image-build", "image-push", "image-artifacthub")
# Artifact Hub's metadata for the image's repository: the tree's file, pushed under the
# tag Artifact Hub reads by a job of the image's file that nothing waits for - after
# container-image in a v* tag's pipeline, and by hand in the default branch's, whose
# rule is ARTIFACTHUB_MANUAL - which pushes nothing until the file holds the ID
# Artifact Hub gave (build/ci/container-image-artifacthub.sh).
ARTIFACTHUB_JOB = "container-image-artifacthub"
ARTIFACTHUB_MANUAL = ('if: $CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH && $CI_COMMIT_REF_PROTECTED == "true"\n'
                      '      when: manual\n      allow_failure: true')
# The regctl commands container-image-artifacthub.sh may run beside its one artifact put
# and regctl version: none of them writes or removes a tag.
ARTIFACTHUB_REGCTL = (("registry", "login"), ("registry", "logout"), ("manifest", "get"), ("artifact", "get"),
                      ("image", "digest"))
ARTIFACTHUB_LINE = re.compile(r"^(?:[ \t]*|#.*|repositoryID: [0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$")
# Jobs that need not begin with REFUSAL, because their own rules match nothing a
# downstream pipeline (source "pipeline", on master) is, or run in one on purpose -
# each with the text that makes it so. None of them is a gate of a tag's pipeline,
# so release need not wait for them either.
EXEMPT = {
    "pages": ("$CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH", "the default branch, whatever started its pipeline, and the feed's schedule"),
    "linux-fuzz": ('$CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "fuzz" &&', "only the fuzz schedule"),
    "linux-coverage": ('$CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "coverage" &&', "only the coverage schedule"),
    "renovate": ('$CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "renovate" &&', "only Renovate's schedule"),
    "image-pins": ('$CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "renovate" &&', "only Renovate's schedule"),
    "sign-off": ('$CI_COMMIT_BRANCH =~ /^batch/ && $CI_COMMIT_REF_PROTECTED == "true"', "only a merge request or a batch* branch"),
    "installer-smoke": ("($CI_COMMIT_TAG =~ /^v/ || $CI_COMMIT_BRANCH =~ /^batch/)", "only a v* tag or a batch* branch"),
}
TAG_ONLY = '$CI_COMMIT_TAG =~ /^v/ && $CI_COMMIT_REF_PROTECTED == "true"'
# What every job but EXEMPT's begins its rules with, when: never: every scheduled
# pipeline, and a downstream pipeline, whose CI_PIPELINE_SOURCE is "pipeline" (nothing of
# this project starts one: the release's feed is deployed in the tag's own pipeline).
REFUSAL = '$CI_PIPELINE_SOURCE == "schedule" || $CI_PIPELINE_SOURCE == "pipeline"'
# The schedules that each run one job of their own, by their description, and the
# file the job is in. pages, which runs for every other schedule on master, refuses these.
ONE_JOB_SCHEDULES = {"fuzz": ("linux-fuzz", "ci"), "coverage": ("linux-coverage", "ci"), "renovate": ("renovate", "ci"), "export": ("forge-export", "export"),
                     "security": ("secret_detection", "security")}
# The jobs of .gitlab-ci.yml that only their schedule runs: no push starts them, so
# the rule that a push changing documentation alone starts nothing does not apply.
SCHEDULE_ONLY_JOBS = [job for job, where in ONE_JOB_SCHEDULES.values() if where == "ci"]
# package-repos, in a file of its own, deploys the feed's site with pages; its rules
# are pages' and nothing else, so it refuses what pages refuses.
REPOS_RULES = "  rules:\n    - !reference [pages, rules]\n"
# The variables that chose those pipelines until 26 September 2026. The project refuses
# them on a schedule, and a rule that still reads one is a second way in.
OLD_ROUTES = ("HM_PIPELINE", "RUN_RENOVATE")
# The YAML anchor of .own-runners' rules:changes paths, which db-matrix names too.
NOT_DOCS_ONLY = "not-docs-only"
# The long gates, which a later push must not cancel half-way.
NOT_INTERRUPTIBLE = ("linux-suite", "windows-gate")
# How a gate that lets a busy bench pass (allow_failure: exit_codes) keeps a v* tag's
# pipeline from doing so: the bench's "busy" exits 10 only outside a tag.
BUSY_GUARD = "$taken -eq 1 -and -not $env:CI_COMMIT_TAG"
# The package jobs whose files a release carries (.gitlab/ci/packages-*.yml), by the
# kind of server that installs each (generate-update-feed.py's installer_names): the
# file the jobs are in, the job that builds the package, the one that makes its SBOM,
# the one that installs it in a clean container of its distribution and architecture,
# and where the build leaves the package, with its SBOM beside it.
CARRIED_PACKAGES = {
    "deb-arm64": {"file": "packages-deb", "build": "deb-ubuntu26.04-arm64", "sbom": "deb-ubuntu26.04-arm64-sbom",
                  "install": "deb-ubuntu26.04-arm64-install", "dir": "packages-deb/ubuntu26.04-arm64/"},
    "rpm-aarch64": {"file": "packages-rpm", "build": "rpm-fedora44-aarch64", "sbom": "rpm-fedora44-aarch64-sbom",
                    "install": "rpm-fedora44-aarch64-install", "dir": "packages-rpm/fedora44-aarch64/generic/"},
}
# The job that builds the ARM64 Windows installer a release carries from 6.3.6
# (.gitlab/ci/packages-windows.yml), where it leaves it with the SHA256SUMS it writes,
# and the folder release-authenticode signs it into.
ARM64_INSTALLER_JOB = "windows-packages-arm64"
# The branch's x64 installer (packages-windows.yml): never in a tag's pipeline, whose x64
# installer is windows-installer's.
BRANCH_X64_INSTALLER_JOB = "windows-packages-x64"
ARM64_INSTALLER_DIR = "packages-windows/arm64/"
ARM64_SIGNED_DIR = "release-authenticode-arm64"
# The folder release-authenticode signs the PowerShell module into, and the job on
# bench150 that holds the build inputs to the signatures the manifest records
# (.gitlab/ci/packages-windows.yml), which every build placing them needs.
MODULE_SIGNED_DIR = "release-authenticode-module"
BUILD_INPUTS_JOB = "build-inputs-authenticode"
# The job that gathers every other file a release carries from the package jobs
# (.gitlab/ci/release-packages.yml): each distribution release's .deb and .rpm, the
# AppImages and the source tarball, each with its SBOM; and the chart's job.
GATHER_JOB = "release-packages"
CHART_JOB = "helm-chart-package"
# The job that makes the source of the libraries linked into the AppImages' runtime,
# and where it leaves it (.gitlab/ci/packages-appimage.yml).
RUNTIME_SOURCES_JOB = "appimage-runtime-sources"
RUNTIME_SOURCES_DIR = "appimage-runtime-sources/"
# And the source of the libraries the AppImages carry in usr/lib.
BUNDLED_SOURCES_JOB = "appimage-bundled-sources"
BUNDLED_SOURCES_DIR = "appimage-bundled-sources/"
# Its rules: made for a v* tag and a batch* branch only, and there by .own-runners'
# rules, in exactly the pipelines the package jobs are made in.
GATHER_REFUSAL = "- if: $CI_COMMIT_TAG !~ /^v/ && $CI_COMMIT_BRANCH !~ /^batch/\n      when: never\n    - !reference [.own-runners, rules]"
# GitLab's ci_needs_size_limit on GitLab.com: a job that needs more makes the whole
# pipeline fail to be created.
NEEDS_LIMIT = 50
# The files that hold jobs, whose needs are counted.
JOB_FILES = ("ci", "security", "image", "export", "packages-deb", "packages-rpm", "packages-appimage", "packages-windows",
             "repos", "release-packages", "helm", "tarball-ci", "rehearsal")
# The release rehearsal (.gitlab/ci/release-rehearsal.yml): its jobs, what each needs,
# the one script each runs, and the rules template every one takes, whose first rule
# refuses a merge request, a tag, master and any branch but batch*.
REHEARSAL_JOBS = {
    "release-rehearsal": (["linux-build", "linux-sbom", "deb-ubuntu26.04-arm64", "deb-ubuntu26.04-arm64-sbom", "rpm-fedora44-aarch64",
                           "rpm-fedora44-aarch64-sbom", GATHER_JOB, CHART_JOB, "windows-packages-x64", "windows-packages-arm64"],
                          ["python3 build/ci/release-chain.py collect --rehearse --out release-assets",
                           "python3 build/ci/release-chain.py sbom --rehearse --out release-sbom"]),
    "release-rehearsal-sign": (["release-rehearsal"], ["sh build/ci/release-sign.sh --rehearse"]),
    "release-rehearsal-verify": (["release-rehearsal", "release-rehearsal-sign"], ["bash build/ci/release-verify.sh --rehearse"]),
}
REHEARSAL_RULES = ".release-rehearsal-rules"
REHEARSAL_REFUSAL = ('- if: $CI_PIPELINE_SOURCE == "merge_request_event" || $CI_COMMIT_TAG || $CI_COMMIT_BRANCH !~ /^batch/ || '
                     '$CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH\n      when: never\n    - !reference [.own-runners, rules]')
# What no job of the rehearsal may name: it reads nothing of the registry or the
# releases and publishes nothing.
NOT_IN_A_REHEARSAL = ("publish", "CI_JOB_TOKEN", "JOB-TOKEN", "release-cli", "glab", "curl", "wget", "api/v4", "PRIVATE-TOKEN")
# The jobs whose artifacts release collects; it waits for every other gate without them.
COLLECTED = ["linux-build", "linux-sbom", "windows-installer", "windows-packages-arm64"] + [
    job[role] for _, job in sorted(CARRIED_PACKAGES.items()) for role in ("build", "sbom")] + [GATHER_JOB, CHART_JOB]
# The security scans whose reports release reads (build/ci/security-gate.py's SCANS):
# needed, not optionally, with their artifacts. semgrep-sast is GitLab's, in
# .gitlab/ci/gitlab-security.yml; the others are the CI file's own, on linix.
SCANNED = ["static-analysis", "semgrep", "dependency-scan", "semgrep-sast"]
# The two of them that fail a pipeline on a finding new to their committed baselines
# (since 6.3.6), and are therefore never allow_failure.
BLOCKING_SCANS = ["static-analysis", "semgrep"]
# The files a change to which is the owner's alone to accept.
OWNED = ["/.gitlab/security-exceptions.yml", "/.gitleaksignore"]
# What secret_detection runs before its analyzer: GitLab's rules, but for the entries of
# their global allowlist that leave out every path under a directory named build, bin,
# obj, target or packages, derived from the image's own file and put where the analyzer
# reads them (build/ci/secret-detection-rules.sh, which build/scan-secrets.py runs the same
# way). It reads no list of paths from the tree: no committed data file can widen what the
# scans leave out, and the script itself is one of land-via-mr.py's GATE_FILES.
SECRET_RULES_STEP = ("    - sh build/ci/secret-detection-rules.sh /gitleaks.toml /tmp/hm-gitleaks.toml\n"
                     "    - cp /tmp/hm-gitleaks.toml /gitleaks.toml\n    - /analyzer run\n")
# The change gates (.gitlab/ci/mr-checks.yml), which every change passes before it
# lands (OpenSSF Baseline OSPS-VM-05.03 and 06.02), and the scan whose report the first
# reads: each in a merge request's pipeline and in a protected batch* branch's, which is
# the one GitLab holds that branch's merge request to.
CHANGE_GATES = {"semgrep-sast-gate": "python3 build/ci/security-gate.py --merge-request auto",
                "mr-dependency-check": "python3 build/ci/mr-dependency-check.py --base auto"}
GATE_SCAN = "semgrep-sast"
GATE_RULES = ('- if: $CI_PIPELINE_SOURCE == "merge_request_event"',
              '- if: $CI_PIPELINE_SOURCE != "schedule" && $CI_PIPELINE_SOURCE != "pipeline" && $CI_COMMIT_BRANCH =~ /^batch/ '
              '&& $CI_COMMIT_REF_PROTECTED == "true"')
GATE_SCAN_BATCH_RULE = ('- if: $CI_PIPELINE_SOURCE != "merge_request_event" && $CI_COMMIT_BRANCH =~ /^batch/ && '
                        '$CI_COMMIT_REF_PROTECTED == "true"')
# The first rule of .gitlab-ci.yml's workflow: a merge request from a protected batch*
# branch of this project runs no pipeline beside the branch's, which runs the gates;
# every other merge request, a fork's from a branch protected there among them, runs its
# own. The branches it names are held to the ones the gates' rules run for.
WORKFLOW_BATCH_RULE = ('- if: $CI_PIPELINE_SOURCE == "merge_request_event" && $CI_MERGE_REQUEST_SOURCE_PROJECT_ID == '
                       '$CI_MERGE_REQUEST_PROJECT_ID && $CI_MERGE_REQUEST_SOURCE_BRANCH_NAME =~ /^batch/ && '
                       '$CI_MERGE_REQUEST_SOURCE_BRANCH_PROTECTED == "true"\n      when: never')
# semgrep-sast writes its own report: what a checkout leaves of one goes first, and
# Semgrep's SARIF, which says what the scan gave up on, is kept beside it.
SAST_SCRIPT = "  script:\n    - rm -f gl-sast-report.json semgrep.sarif\n    - /analyzer run\n"
# The variables by which a CI file would narrow semgrep-sast unseen, which none may
# define but semgrep-sast's own job, SAST_SEMGREP_EXCLUDED_PATHS to nothing: a second
# list of exclusions the analyzer passes to Semgrep, a ruleset from another repository,
# the analyzers left out, and clang-tidy's checks for static-analysis.
SAST_NARROWING_VARIABLES = ("SAST_SEMGREP_EXCLUDED_PATHS", "SAST_RULESET_GIT_REFERENCE", "SAST_EXCLUDED_ANALYZERS",
                            "HM_TIDY_CHECKS")
# What the tree may not carry: a .semgrepignore (GitLab's analyzer includes the root's,
# Semgrep reads one in a directory), GitLab's custom ruleset, and a scan's report.
NOT_IN_THE_TREE = re.compile(r"(?:^|/)\.semgrepignore[^/]*$|^\.gitlab/sast-ruleset\.toml$|^gl-sast-report\.json$|"
                             r"^\.gitlab/secret-detection-ruleset\.toml$|"
                             r"^semgrep\.sarif$|^security/")
# What GitLab may fail a gate's job for that is no answer of its own, retried (.gate-retry
# in .gitlab/ci/gitlab-security.yml); and the gates' scripts' "could not read", exit 2.
GATE_RETRY_REASONS = ("runner_system_failure", "runner_external_dependency_failure", "runner_interrupted",
                      "scheduler_failure", "api_failure", "stuck_pending_with_matching_runners", "no_updates_running")
# semgrep-sast's analyzer as the project's registry holds it (build/ci/mirror-semgrep.sh):
# tagged with the analyzer's version and the first twelve hex digits of the digest it is
# named by.
MIRRORED_SAST = re.compile(r"^    name: registry\.gitlab\.com/hmailserver/hmailserver/mirror/semgrep:"
                           r"[0-9]+(?:\.[0-9]+)*-([0-9a-f]{12})@sha256:([0-9a-f]{64})\s*$", re.MULTILINE)
# secret_detection's analyzer as the project's registry holds it (build/ci/mirror-secrets.sh),
# tagged as semgrep-sast's is, on the one image: line build/scan-secrets.py takes the
# landing's scanner from.
MIRRORED_SECRETS = re.compile(r"^  image: registry\.gitlab\.com/hmailserver/hmailserver/mirror/secrets:"
                              r"[0-9]+(?:\.[0-9]+)*-([0-9a-f]{12})@sha256:([0-9a-f]{64})\s*$", re.MULTILINE)
# container_scanning's scanner: Trivy, in Docker Hub's aquasec/trivy by its release and
# digest, on the job's image's name: line - dependency-scan's, which the check holds it
# to, so that Renovate moves the two together. Not GitLab's analyzer, which GitLab
# rebuilds daily and stops serving a digest of within days, and which may not be copied
# into the project's registry, as the two above are: the image carries code under
# GitLab's EE licence and a database built with the GitLab Advisory Database, whose
# terms allow neither to be published (.gitlab/ci/container-image.yml's header).
# GITLAB_SCANNER finds that analyzer, or GitLab's template, on a line of a CI file that
# is not a comment.
CONTAINER_SCANNER = re.compile(r"^    name: (aquasec/trivy:[0-9]+(?:\.[0-9]+)*@sha256:[0-9a-f]{64})\s*$", re.MULTILINE)
GITLAB_SCANNER = re.compile(r"security-products/container-scanning|Container-Scanning\.gitlab-ci\.yml")
# What build/ci/container-scan.sh must hold: each platform's image by its digest, the
# project's alone, from container-image's dotenv report; Trivy's vulnerability scan of
# that platform; the database from where dependency-scan takes it; and the report in
# GitLab's format, where the job keeps it.
CONTAINER_SCAN_SCRIPT = ("REPOSITORY=registry.gitlab.com/hmailserver/hmailserver\n",
                         'DATABASES="mirror.gcr.io/aquasec/trivy-db:2 public.ecr.aws/aquasecurity/trivy-db:2"\n',
                         "OUT=container-scan\n",
                         'digest=${image#"$REPOSITORY@sha256:"}\n',
                         '[ "${#digest}" -eq 64 ] || fail 2',
                         'trivy image --skip-db-update --scanners vuln --platform "linux/$arch" --format json --output "$report"',
                         'grep -qF "sha256:$digest" "$report" || fail 1',
                         'trivy convert --format template --template @/contrib/gitlab.tpl --output "$OUT/gl-container-scanning-report.json"')
# The scans on linix that judge a batch as a whole: on a protected batch* branch their
# rules compare the tree with master, not with the push before.
WHOLE_CHANGE_SCANS = ["static-analysis", "semgrep", "ci-image-packages", "dependency-scan"]
BATCH_COMPARE = ('- if: $CI_PIPELINE_SOURCE != "merge_request_event" && $CI_COMMIT_REF_PROTECTED == "true" && '
                 '$CI_COMMIT_BRANCH =~ /^batch/\n      changes:\n        paths: *%s\n        compare_to: refs/heads/master\n'
                 '    - if: $CI_COMMIT_BRANCH =~ /^batch/\n      when: never\n' % NOT_DOCS_ONLY)
# The jobs build/land-via-mr.py requires to have succeeded before it lands a batch, as
# it names them - a module-level literal sequence of job names each - and the jobs each
# must hold: the change gates, their scan, and checkers, which this runs in - a CI file
# that let it fail would take every rule here away unseen - always (GATE_JOBS); the two
# blocking scans on linix unless the batch changes documents alone (SOURCE_SCAN_JOBS),
# which its documents_only tells as .own-runners' paths: *not-docs-only does.
LANDING_REQUIRED = {"GATE_JOBS": sorted(CHANGE_GATES) + [GATE_SCAN, "checkers"], "SOURCE_SCAN_JOBS": BLOCKING_SCANS}
# The scripts build/land-via-mr.py runs before it pushes a batch - the sign-off check and
# the secret scan, and the script that makes the scan's rules, which secret_detection runs
# too - which its GATE_FILES must name with the CI files the gates run in.
LANDING_RUNS = ("build/sign-off.py", "build/scan-secrets.py", "build/ci/secret-detection-rules.sh")
# What a landing runs from its own checkout, which its CHECKOUT_COPIES must hold to the
# base - or, on --own-copies, to the batch's tip - before it runs any: those scripts, the
# landing script itself, and the CI file whose secret_detection image the secret scan
# runs in.
LANDING_CHECKOUT = LANDING_RUNS + ("build/land-via-mr.py", ".gitlab/ci/gitlab-security.yml")
LANDING_DOCUMENTS = "documents_only"
# Paths beside the tree's own on which documents_only and the CI files must agree: a
# document at the root and under a dot-directory, which start no scan, and one under
# another directory, code, and a file named like a document's extension alone.
LANDING_SAMPLE_PATHS = ["README.md", ".gitlab/notes.md", ".github/a/b.md", "docs/a.md", "a.md.txt", "a.txt", "md",
                        "Makefile", ".gitlab/ci/x.yml", "hmailserver/docs/Notes.md"]
COSIGN_IMAGE = re.compile(r'^\s*HM_COSIGN_IMAGE:\s*"?(\S+?)"?\s*$', re.MULTILINE)
PINNED_COSIGN = re.compile(r"^gcr\.io/projectsigstore/cosign:[A-Za-z0-9._\-]+@sha256:[0-9a-f]{64}$")
PLACEHOLDER_COSIGN = re.compile(r"^gcr\.io/projectsigstore/cosign:[A-Za-z0-9._\-]+@sha256:PIN-THE-DIGEST$")
RESERVED = {"stages", "workflow", "default", "variables", "include"}
# The project's paths on GitLab. It began in the owner's personal namespace
# (FIRST_PATH), where 6.3.5 was signed, and moved on 29 September 2026 into the group
# the owner reserved for it (GROUP_PATH), where every release from 6.3.6 is signed.
# SigstoreVerifier.cpp trusts the pipeline at both - at the first for what was signed
# there, at the group's with the project's id required, which every server from 6.3.4
# on knew before the move - and the release scripts sign from the group's path alone:
# a pipeline at the first path now is not this project's.
FIRST_PATH = "https://gitlab.com/Progressiverobot/hmailserver"
GROUP_PATH = "https://gitlab.com/hmailserver/hmailserver"
IDENTITY_TAIL = "//.gitlab-ci.yml@refs/tags/"
# The identity release-sign.sh and release-verify.sh expect: the repository's, for
# the ref - refs/tags/<tag> for a release, refs/heads/<branch> in a rehearsal alone.
SCRIPT_IDENTITY = 'identity="${repository}//.gitlab-ci.yml@${ref}"'
TAG_REF = 'ref="refs/tags/${tag}"'
BRANCH_REF = 'ref="refs/heads/${branch}"'
# The container image's signing script signs for a tag and nothing else.
TAG_SCRIPT_IDENTITY = 'identity="${repository}%s${tag}"' % IDENTITY_TAIL


def _load(relative, name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, relative))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read(relative):
    with io.open(os.path.join(ROOT, relative), encoding="utf-8") as f:
        return f.read().replace("\r\n", "\n")


def docs_only(path):
    """The Markdown files a push may change without starting the project's own
    runners: at the root of the tree, and under .gitlab/ (the issue and merge request
    templates). Every other one counts as code: hmailserver/docs/*.md is compiled
    into the server, and the READMEs further down are read by tests and packaging."""
    return path.endswith(".md") and ("/" not in path or path.startswith(".gitlab/"))


def glob_regex(glob):
    """A rules:changes path as GitLab matches it - Ruby's File.fnmatch with
    FNM_PATHNAME, FNM_DOTMATCH and FNM_EXTGLOB - for what the patterns here use:
    "**/" is any number of directories, none included; "*" and "?" stay inside one
    name and match a leading dot; "[!...]" is one character not in the set, never a
    "/". Braces, the one thing FNM_EXTGLOB adds, are refused rather than guessed at."""
    out, i = "", 0
    while i < len(glob):
        if glob.startswith("**/", i):
            out += "(?:[^/]*/)*"
            i += 3
        elif glob[i] == "*":
            out += "[^/]*"
            i += 1
        elif glob[i] == "?":
            out += "[^/]"
            i += 1
        elif glob[i] == "[":
            end = glob.index("]", i + 2)
            body = glob[i + 1:end]
            if body[0] in "!^":
                out += "[^" + re.escape(body[1:]) + "/]"
            else:
                out += "[" + re.escape(body) + "]"
            i = end + 1
        elif glob[i] in "{}":
            raise ValueError("%s: braces are not read here" % glob)
        else:
            out += re.escape(glob[i])
            i += 1
    return re.compile("^" + out + "$")


def tree_files():
    """Every file of the tree, as git names it: what a push can change."""
    try:
        listed = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True).stdout
        return [name for name in listed.decode("utf-8").split("\0") if name]
    except (OSError, subprocess.CalledProcessError):
        found = []
        for base, dirs, files in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d != ".git"]
            for name in files:
                found.append(os.path.relpath(os.path.join(base, name), ROOT).replace(os.sep, "/"))
        return found


def blocks(ci):
    """Every top-level key of the CI file and its text, comments left out."""
    found = {}
    name = None
    lines = []
    for line in ci.split("\n"):
        match = re.match(r"^([A-Za-z0-9_.\-]+):\s*$", line)
        if match:
            if name:
                found[name] = "\n".join(lines)
            name, lines = match.group(1), []
            continue
        if name and not line.lstrip().startswith("#"):
            lines.append(line)
    if name:
        found[name] = "\n".join(lines)
    return found


def rules_text(name, jobs):
    """A job's rules as GitLab resolves them: its own, what it extends, and every
    !reference it names."""
    text = jobs.get(name, "")
    seen = set()
    pending = [text]
    resolved = []
    while pending:
        chunk = pending.pop()
        resolved.append(chunk)
        for reference in re.findall(r"!reference \[([^,\]]+), rules\]", chunk):
            if reference not in seen:
                seen.add(reference)
                pending.append(jobs.get(reference, ""))
        match = re.search(r"^  extends:\s*(\S+)\s*$", chunk, re.MULTILINE)
        if match and match.group(1) not in seen and not re.search(r"^  rules:", chunk, re.MULTILINE):
            seen.add(match.group(1))
            pending.append(jobs.get(match.group(1), ""))
        elif match and match.group(1) not in seen:
            # A job's own rules replace its template's; the template may still
            # extend another whose rules it does not replace. Nothing to add.
            seen.add(match.group(1))
    return "\n".join(resolved)


def inherited(name, jobs):
    """A job's text and the text of every template it extends, nearest first."""
    texts = []
    seen = set()
    while name and name not in seen and name in jobs:
        seen.add(name)
        texts.append(jobs[name])
        match = re.search(r"^  extends:\s*(\S+)\s*$", jobs[name], re.MULTILINE)
        name = match.group(1) if match else None
    return "\n".join(texts)


def gates(jobs):
    """Every job of .gitlab-ci.yml that can fail a v* tag's pipeline before release: not
    a release job, not EXEMPT (none of which is a gate of a tag), not allow_failure, not
    manual. The included files' jobs are not read: the package jobs there are not
    release gates, but for CARRIED_PACKAGES', which check() holds release to itself."""
    found = []
    for name in sorted(jobs):
        if name.startswith(".") or name in RESERVED or name in RELEASE_JOBS or name in EXEMPT:
            continue
        text = inherited(name, jobs) + "\n" + rules_text(name, jobs)
        if "allow_failure: true" in text or "when: manual" in text:
            continue
        found.append(name)
    return found


def template_order(name, jobs, seen=None):
    """A job and every template it extends, in either spelling of extends, in the order
    GitLab lets them decide a key: the job, then the last template named and what that
    one extends, then the one before it."""
    seen = set() if seen is None else seen
    if name in seen or name not in jobs:
        return []
    seen.add(name)
    order = [name]
    extends = re.search(r"^  extends:\s*(.+?)\s*$", jobs[name], re.MULTILINE)
    if extends:
        for parent in reversed([n.strip() for n in extends.group(1).strip("[]").split(",") if n.strip()]):
            order += template_order(parent, jobs, seen)
    return order


def rule_items(text, jobs, seen=None):
    """The rules of a block, each item's text, with every !reference [X, rules] replaced
    by X's own items."""
    seen = set() if seen is None else seen
    match = re.search(r"^  rules:\s*\n(.*?)(?=^  [A-Za-z_]|\Z)", text, re.MULTILINE | re.DOTALL)
    items = []
    if not match:
        return items
    for item in re.split(r"^    - ", match.group(1), flags=re.MULTILINE)[1:]:
        reference = re.match(r"!reference \[([^,\]]+), rules\]", item.strip())
        if reference:
            if reference.group(1) not in seen:
                seen.add(reference.group(1))
                items += rule_items(jobs.get(reference.group(1), ""), jobs, seen)
        else:
            items.append(item)
    return items


def unread_keys(text, level):
    """The lines of a block's text, whose keys are level spaces in, that this checker
    does not read as keys: each line there or further out that is not a bare key - a
    name of small letters and underscores, unquoted, its colon straight after it, then a
    space, a tab or the line's end. A quoted key, a space before the colon, a merge key
    (<<), a list item and a document marker are among them, and GitLab's YAML reader
    reads each."""
    return [line.strip() for line in text.split("\n")
            if line.strip() and len(line) - len(line.lstrip(" ")) <= level
            and not (line[:level] == " " * level and re.match(r"[a-z_]+:(?:[ \t]|$)", line[level:]))]


def unread_rules(name, jobs, seen=None):
    """What this checker does not read of the rules the block name of jobs takes, as
    rule_items reads them: a second rules: key, which GitLab's YAML reader takes and
    rule_items does not; anything after rules: on its line; a list that is not indented
    four; a rule that is not a list item of bare keys or !reference [X, rules] - a rule
    in flow style, an alias, a !reference to anything but X's rules list; and an X that
    is not a block of jobs, or whose own keys unread_keys finds unread. Each is "where:
    what"; [] when every rule is read."""
    seen = set() if seen is None else seen
    seen.add(name)
    text = jobs[name]
    found = []
    heads = re.findall(r"^  rules:(.*)$", text, re.MULTILINE)
    if len(heads) > 1:
        found.append("%s: a second rules: key" % name)
    if heads and heads[0].strip():
        found.append("%s: rules:%s" % (name, heads[0]))
    body = re.search(r"^  rules:\s*\n(.*?)(?=^  [A-Za-z_]|\Z)", text, re.MULTILINE | re.DOTALL)
    items = re.split(r"^    - ", body.group(1), flags=re.MULTILINE) if body else [""]
    if items[0].strip():
        found.append("%s: %s" % (name, items[0].strip().split("\n")[0]))
    for item in items[1:]:
        reference = re.match(r"!reference \[([A-Za-z0-9_.\-]+), rules\]$", item.strip())
        if not reference:
            first, _, rest = item.partition("\n")
            if not re.match(r"[a-z_]+:(?:[ \t]|$)", first):
                found.append("%s: - %s" % (name, first.strip()))
            found += ["%s: %s" % (name, line) for line in unread_keys(rest, 6)]
        elif reference.group(1) not in jobs:
            found.append("%s: !reference [%s, rules], of a block this does not read" % (name, reference.group(1)))
        elif reference.group(1) not in seen:
            found += ["%s: %s" % (reference.group(1), line) for line in unread_keys(jobs[reference.group(1)], 2)]
            found += unread_rules(reference.group(1), jobs, seen)
    return found


def tag_escapes(name, jobs):
    """How a job, in a v* tag's pipeline, could let a job that needs it start without its
    having passed: allow_failure: true, allow_failure with exit_codes (a need on a job
    that failed with one of them is met, as on one allowed to fail at all) or when:
    manual in the job or a template it extends, or in a rule of the rules it takes that
    a v* tag can match. A rule whose if: asks for $CI_COMMIT_TAG !~ /^v/, with && alone,
    is one no tag matches. jobs holds every block the job, its templates and their
    references can name."""
    found = []
    order = template_order(name, jobs)
    for current in order:
        for line in jobs[current].split("\n"):
            if re.match(r"^  (allow_failure:\s*true|when:\s*manual)\s*$", line):
                found.append("%s says %s" % (current, line.strip()))
        for match in re.finditer(r"^  allow_failure:[ \t]*(?:\{[^}\n]*exit_codes[^}\n]*\}|\n    exit_codes:[^\n]*)", jobs[current],
                                 re.MULTILINE):
            found.append("%s says %s" % (current, " ".join(match.group(0).split())))
    deciding = [current for current in order if re.search(r"^  rules:", jobs[current], re.MULTILINE)]
    for item in (rule_items(jobs[deciding[0]], jobs) if deciding else []):
        if not re.search(r"^\s*(allow_failure:\s*true|when:\s*manual)\s*$", item, re.MULTILINE):
            continue
        condition = re.match(r"if:\s*(.+)", item.strip())
        condition = condition.group(1) if condition else ""
        if "$CI_COMMIT_TAG !~ /^v/" not in condition or "||" in condition:
            found.append("a rule a v* tag can match, %r, makes it %s" % (
                condition or item.strip().split("\n")[0], "manual" if "manual" in item else "allowed to fail"))
    return found


def takes_no_github(name, jobs):
    """Whether a job runs .no-github's before_script (.gitlab/ci/no-github.yml), which
    keeps GitHub out of the job itself. The block that decides the job's before_script
    is the first of the job and its templates, in the order GitLab lets them decide a
    key, that has one: it must be .no-github, or begin with
    !reference [.no-github, before_script]. A job's own before_script, or a nearer
    template's, replaces what extends brings, whole, so .no-github in extends behind one
    keeps nothing out. jobs holds every block the job and its templates can name."""
    for current in template_order(name, jobs):
        match = re.search(r"^  before_script:(.*)$", jobs[current], re.MULTILINE)
        if not match:
            continue
        if current == ".no-github":
            return True
        first = match.group(1).strip()
        if not first:
            # The key's list: its first item, before the next key of the block.
            for line in jobs[current][match.end():].split("\n")[1:]:
                if re.match(r"^ {0,2}\S", line):
                    break
                if line.startswith("    - "):
                    first = line[6:].strip()
                    break
        return first.lstrip("[").startswith("!reference [.no-github, before_script]")
    return False


def need_entries(block):
    """A job's needs, each name with the text of its entry."""
    match = re.search(r"^  needs:\s*\n(.*?)(?=^  [a-z_]+:|\Z)", block, re.MULTILINE | re.DOTALL)
    entries = {}
    if not match:
        return entries
    for entry in re.split(r"^    - ", match.group(1), flags=re.MULTILINE):
        head = re.match(r"(?:job:\s*)?([a-z][a-z0-9._\-]*)", entry)
        if head:
            entries[head.group(1)] = entry
    return entries


def needs(block):
    """The job names in a job's needs, in either spelling."""
    match = re.search(r"^  needs:(.*?)(?=^  [a-z_]+:|\Z)", block, re.MULTILINE | re.DOTALL)
    if not match:
        return []
    inline = re.match(r"^\s*\[([^\]]*)\]", match.group(1))
    if inline:
        return [n.strip() for n in inline.group(1).split(",") if n.strip()]
    found = []
    for line in match.group(1).split("\n"):
        item = re.match(r"^\s*-\s*(?:job:\s*)?([a-z][a-z0-9._\-]*)\s*$", line)
        if item:
            found.append(item.group(1))
    return found


class _Done(object):
    def __init__(self, code, out="", err=""):
        self.returncode, self.stdout, self.stderr = code, out, err


def _key(seed):
    """An ed25519 public key, as allowed_signers writes it, that no one holds."""
    return base64.b64encode(b"\x00\x00\x00\x0bssh-ed25519\x00\x00\x00\x20" + bytes((seed + i) % 256 for i in range(32))).decode("ascii")


def probe_tag_check(chain):
    """chain.check_tag, run against a git that answers as the release job's shallow
    checkout would: master's history and allow-list are there once master is fetched
    by name, the tagged tree's own allow-list names another key, and verify-tag
    reads whichever allow-list it is given."""
    key_a, key_b = _key(1), _key(101)
    list_a = "owner@example.org ssh-ed25519 %s\n" % key_a
    list_b = "someone@example.org ssh-ed25519 %s\n" % key_b
    commit = "c" * 40
    problems = []
    work = tempfile.mkdtemp(prefix="hm-tag-probe-")
    try:
        os.makedirs(os.path.join(work, ".gitlab"))
        with io.open(os.path.join(work, ".gitlab", "allowed_signers"), "w", encoding="utf-8") as f:
            f.write(list_b)

        def git(on_master=True, master_list=list_a, signed_by=key_a, names_key=None):
            state = {"master": False}

            def answer(arguments, **kwargs):
                if "cat-file" in arguments:
                    return _Done(0, "tag\n")
                if "rev-parse" in arguments:
                    return _Done(0, commit + "\n")
                if "fetch" in arguments:
                    if "+refs/heads/master:refs/remotes/origin/master" in arguments:
                        state["master"] = True
                    return _Done(0)
                if "merge-base" in arguments:
                    return _Done(0 if on_master and state["master"] and "refs/remotes/origin/master" in arguments else 1)
                if "show" in arguments:
                    if state["master"] and arguments[-1] == "refs/remotes/origin/master:.gitlab/allowed_signers":
                        return _Done(0, master_list)
                    return _Done(128, "", "fatal: invalid object name")
                if "verify-tag" in arguments:
                    given = [a.split("=", 1)[1] for a in arguments if a.startswith("gpg.ssh.allowedSignersFile=")]
                    path = given[0] if given else ""
                    path = path if os.path.isabs(path) else os.path.join(work, path)
                    try:
                        with io.open(path, encoding="utf-8") as f:
                            listed = f.read()
                    except (OSError, IOError):
                        return _Done(1, "", "no allowed signers file")
                    for line in listed.splitlines():
                        fields = line.split()
                        if len(fields) >= 3 and fields[2] == signed_by:
                            return _Done(0, "", 'Good "git" signature for %s with ED25519 key %s\n' % (
                                fields[0], chain.fingerprint(names_key or signed_by)))
                    return _Done(1, "", "no principal matched")
                return _Done(0)
            return answer

        cases = [
            ("a tag master's one key signed, on master's history", git(), False),
            ("a tag signed by a key the tagged tree's own allow-list lists and master's does not", git(signed_by=key_b), True),
            ("a tag on a commit that is not on master", git(on_master=False), True),
            ("a tag signed by a second key master's list holds", git(master_list=list_a + list_b, signed_by=key_b), True),
            ("a tag signed by a key other than the one git was given", git(names_key=key_b), True),
        ]
        for what, run, refuse in cases:
            try:
                found = chain.check_tag(work, "v9.9.9", commit, run)
            except Exception as e:  # a probe reports, it does not stop the checker
                found = None
                problems.append("build/ci/release-chain.py's tag check fails on %s: %r" % (what, e))
            if found is None:
                continue
            if refuse and not found:
                problems.append("build/ci/release-chain.py's tag check accepts %s" % what)
            if not refuse and found:
                problems.append("build/ci/release-chain.py's tag check refuses %s: %s" % (what, found))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return problems


def probe_registry(chain, feed):
    """chain.check_registry_before_signing, collect's look at the registry, against
    one that holds files of the version already."""
    version = "9.9.9"
    names = feed.installer_names(version)
    digests = dict((name, "a" * 64) for name in chain.collected_assets(version))

    class Registry(object):
        def __init__(self, *held):
            self.files = [{"id": i + 1, "file_name": n, "file_sha256": s} for i, (n, s) in enumerate(held)]

        def feed_get(self, url):
            path = urllib.parse.urlparse(url).path[len("/api/v4/projects/" + feed.PROJECT_ID):]
            if path == "/packages":
                return json.dumps([{"id": 7, "name": chain.PACKAGE, "version": version}]).encode("utf-8"), {}
            if path == "/packages/7/package_files":
                return json.dumps(self.files).encode("utf-8"), {}
            raise feed.FeedError("%s is not asked for here" % path)

    cases = [
        ("an empty registry", Registry(), False),
        ("the Linux files, the sums and the SBOMs with the same bytes, as a publish that stopped part-way leaves them",
         Registry(*[(n, "a" * 64) for n in digests if n not in chain.made_after_collect(version)]), False),
        ("a .deb with other bytes", Registry((names["deb-amd64"], "b" * 64)), True),
        ("a .deb held twice, the older with other bytes", Registry((names["deb-amd64"], "b" * 64), (names["deb-amd64"], "a" * 64)), True),
        ("the installer, which Authenticode makes after collect", Registry((names["windows"], "a" * 64)), True),
        ("the installer's SBOM, which release-sbom makes after collect", Registry((chain.sbom_name(names["windows"]), "a" * 64)), True),
        ("the ARM64 installer, which Authenticode makes after collect", Registry((names["windows-arm64"], "a" * 64)), True),
        ("the ARM64 installer's SBOM, which release-sbom makes after collect",
         Registry((chain.sbom_name(names["windows-arm64"]), "a" * 64)), True),
        ("the PowerShell module's script, which Authenticode makes after collect", Registry(("HmailServer.psm1", "a" * 64)), True),
        ("a bundle, which Sigstore makes after collect", Registry(("hmailserver.spdx.json.cosign.bundle", "c" * 64)), True),
        ("a bundle's .sigstore.json copy, which release-sign makes", Registry(("hmailserver.spdx.json.sigstore.json", "c" * 64)), True),
        ("the provenance, which release-sign attests", Registry((chain.provenance_name(version), "c" * 64)), True),
    ]
    problems = []
    for what, registry, refuse in cases:
        try:
            found = chain.check_registry_before_signing(registry, version, digests)
        except Exception as e:  # a probe reports, it does not stop the checker
            problems.append("build/ci/release-chain.py's collect fails on a registry holding %s: %r" % (what, e))
            continue
        if refuse and not found:
            problems.append("build/ci/release-chain.py's collect takes a registry holding %s" % what)
        if not refuse and found:
            problems.append("build/ci/release-chain.py's collect refuses a registry holding %s: %s" % (what, found))
    return problems


# A permanent download address as the tree writes one: GitLab's for the latest
# release, the name after it perhaps on the next line of a document.
PERMALINK = re.compile(r"/-/releases/permalink/latest/downloads/\s*([A-Za-z0-9_.\-]+)")


def permalink_texts():
    """{path: text} of each file of the tree that writes GitLab's permanent download
    address, but the generated help pages (made from the documents)."""
    texts = {}
    for name in tree_files():
        if name.endswith("HelpPagesData.cpp") or name.startswith(("libraries/", "fuzz/corpus")):
            continue
        try:
            with open(os.path.join(ROOT, name), "rb") as f:
                data = f.read()
        except OSError:
            continue
        if b"/-/releases/permalink/latest/downloads/" not in data:
            continue
        try:
            texts[name] = data.decode("utf-8")
        except UnicodeDecodeError:
            continue
    return texts


def link_path_problems(chain, documents):
    """The release links' paths (direct_asset_path), which GitLab serves at
    /-/releases/<tag>/downloads<path> and the latest release's at
    /-/releases/permalink/latest/downloads<path>: each is one slash and the file's
    permanent name (release-chain.py's permalink_name), never under /downloads/ -
    GitLab puts that in front itself, and 6.3.5's links, whose paths said
    /downloads/<name>, were each a 404 until they were corrected by hand - and a
    file's bundle and copy are at its own path with the suffix added, so that the
    latest release's installer and the bundle that verifies it are both at
    permalink/latest; release-chain.py refuses a path under /downloads/ before
    anything is uploaded; and every permanent address the tree writes (README,
    Channels.md, the OBS _service, the scripts) names a path a release's links have."""
    problems = []
    version = "9.9.9"
    files = chain.release_files(version)
    links = chain.release_links(version, files)
    paths = dict((link["name"], link["direct_asset_path"]) for link in links)
    for name, path in sorted(paths.items()):
        if path != "/" + chain.permalink_name(name, version) or path.startswith("/downloads/"):
            problems.append("build/ci/release-chain.py links %s at the path %s, which GitLab serves at %s: a link's path is "
                            "one slash and the file's permanent name, /%s (GitLab puts /-/releases/<tag>/downloads in front "
                            "itself; 6.3.5's /downloads/<name> paths were 404s)" % (
                                name, path, chain.download_url("<tag>", path), chain.permalink_name(name, version)))
    for name in chain.primary_assets(version):
        for suffix in (chain.BUNDLE, chain.BUNDLE_COPY):
            if name in paths and paths.get(name + suffix) != paths[name] + suffix:
                problems.append("build/ci/release-chain.py links %s at %s, not beside %s at %s%s: the latest release's file "
                                "and the bundle that verifies it are no longer both at permalink/latest under one name" % (
                                    name + suffix, paths.get(name + suffix), name, paths[name], suffix))
    if not any("/downloads/downloads/" in p for p in chain.link_problems([{"name": "a", "url": "u", "direct_asset_path": "/downloads/a"}])):
        problems.append("build/ci/release-chain.py's link_problems takes a link whose path begins /downloads/, which GitLab "
                        "would serve at .../downloads/downloads/<name>: publish would upload every file and create a "
                        "release whose every link is a 404, as 6.3.5's were")
    served = set(path[1:] for path in paths.values())
    for document, text in sorted(documents.items()):
        for found in PERMALINK.finditer(text):
            name = found.group(1).rstrip(".,")
            if name not in served:
                problems.append("%s gives the address .../-/releases/permalink/latest/downloads/%s, which no link of a "
                                "release has: the paths are %s" % (document, name, ", ".join(sorted(
                                    path for path in served if "." in path and not path.endswith((chain.BUNDLE, chain.BUNDLE_COPY)))[:6]) + ", ..."))
    return problems


def function_text(source, name):
    """The text of a top-level function of a Python file."""
    match = re.search(r"^def %s\(.*?(?=^def |^class |^# -{10,}|\Z)" % re.escape(name), source, re.MULTILINE | re.DOTALL)
    return match.group(0) if match else ""


def check(texts, chain, feed):
    problems = []
    notes = []
    ci = texts["ci"]
    jobs = blocks(ci)

    for name in RELEASE_JOBS:
        if name not in jobs:
            problems.append(".gitlab-ci.yml has no %s job: the release chain is %s" % (name, ", ".join(RELEASE_JOBS)))
    if problems:
        return problems, notes

    # The rules every release job runs under: a protected v* tag, never a merge request.
    release_rules = jobs.get(".release-rules", "")
    if TAG_ONLY not in release_rules or 'merge_request_event' not in release_rules or REFUSAL not in release_rules:
        problems.append(".release-rules no longer holds the release jobs to a protected v* tag, refusing merge requests, "
                        "a schedule and a downstream pipeline (%s)" % REFUSAL)
    for name in RELEASE_JOBS:
        text = rules_text(name, jobs)
        if TAG_ONLY not in text:
            problems.append("%s can run for something that is not a protected v* tag: its rules must be .release-rules" % name)
        if re.search(r"^  trigger:", jobs[name], re.MULTILINE):
            problems.append("%s is a trigger: a child or downstream pipeline's ID token has no ci_config_ref_uri, and nothing in it can be signed" % name)
    for name in ("release", "release-publish"):
        if not re.search(r"when: manual\s*\n\s*allow_failure: false", jobs[name]):
            problems.append("%s must wait for a person (when: manual) and hold the jobs after it until then (allow_failure: false)" % name)

    # The order: Authenticode, then Sigstore over the bytes that ship, then the
    # server's verifier, then the upload, then the feed.
    # release waits for every gate: a manual job can be started as soon as its own
    # needs have finished, so a gate it does not need can be released past.
    carried = [job[role] for _, job in sorted(CARRIED_PACKAGES.items()) for role in ("build", "sbom", "install")]
    order = {
        "release": gates(jobs) + ["windows-installer", ARM64_INSTALLER_JOB] + carried + [GATHER_JOB, CHART_JOB],
        "release-authenticode": ["release"],
        "release-sbom": ["release", "release-authenticode"],
        "release-sign": ["release", "release-authenticode", "release-sbom"],
        "release-verify": ["release", "release-sign"],
        "release-publish": ["release-sign", "release-verify"],
        "windows-installer": ["windows-gate"],
    }
    for name, wanted in sorted(order.items()):
        have = needs(jobs[name])
        for need in wanted:
            if need not in have:
                problems.append("%s does not need %s (it needs %s)" % (name, need, ", ".join(have) or "nothing"))

    # release brings only the artifacts it collects: a gate's logs are not a release's.
    for name, entry in sorted(need_entries(jobs["release"]).items()):
        if name in COLLECTED and not re.search(r"artifacts: true", entry):
            problems.append("release does not bring %s's artifacts, which hold a file the release carries" % name)
        if name not in COLLECTED + SCANNED and not re.search(r"artifacts: false", entry):
            problems.append("release brings %s's artifacts, which it does not collect: say artifacts: false" % name)

    # The packages a release carries from the package jobs: built, described and
    # installed before release starts, and collected from where their build leaves them.
    release_entries = need_entries(jobs["release"])
    for platform, job in sorted(CARRIED_PACKAGES.items()):
        found = blocks(texts[job["file"]])
        name = feed.installer_names("9.9.9")[platform]
        for role, does in (("build", "builds"), ("sbom", "makes the SBOM of"), ("install", "installs")):
            if job[role] not in found:
                problems.append("%s has no job %s, which %s %s, a file the release carries" % (FILES[job["file"]], job[role], does, name))
            entry = release_entries.get(job[role])
            if entry is not None and re.search(r"optional: true", entry):
                problems.append("release needs %s optionally: a tag's pipeline without it would release %s past it" % (job[role], name))
        for role in ("sbom", "install"):
            if job[role] in found and job["build"] not in needs(found[job[role]]):
                problems.append("%s does not need %s, whose package it %s" % (
                    job[role], job["build"], "describes" if role == "sbom" else "installs"))
        # A job release needs that may fail, or waits for a press, is no gate: GitLab
        # starts release after it failed, or before it ran.
        named = dict(jobs, **found)
        for role in ("build", "sbom", "install"):
            if job[role] in found:
                for escape in tag_escapes(job[role], named):
                    problems.append("%s can let release start in a v* tag's pipeline without its having passed (%s): "
                                    "release would carry %s past it" % (job[role], escape, name))
        sbom_job = found.get(job["sbom"], "")
        if "bash build/ci/packages/package-sbom.sh " + job["dir"] not in sbom_job or not re.search(
                r"^      - %s\*\.cdx\.json\s*$" % re.escape(job["dir"]), sbom_job, re.MULTILINE):
            problems.append("%s does not run build/ci/packages/package-sbom.sh on the package in %s and keep %s*.cdx.json, "
                            "the SBOM release collects" % (job["sbom"], job["dir"], job["dir"]))
        where = chain.sources("9.9.9")
        for file_name in (name, chain.sbom_name(name)):
            if (where.get(file_name) or "").replace(os.sep, "/") != job["dir"] + file_name:
                problems.append("build/ci/release-chain.py collects %s from %s, not from %s%s, where %s leaves it" % (
                    file_name, where.get(file_name), job["dir"], file_name, job["sbom"] if file_name != name else job["build"]))
        if name not in chain.compiled_assets("9.9.9"):
            problems.append("build/ci/release-chain.py's compiled_assets does not carry %s, which %s builds for it" % (name, job["build"]))

    # Every compiled file with its own SBOM: made by the jobs, signed with the rest,
    # and held to its file before the release is published.
    carried = chain.expected_assets("9.9.9")
    packages = [n for n in carried if chain.link_type(n) == "package"]
    compiled = chain.compiled_assets("9.9.9")
    # Every package goes out with its SBOM, and so does the source tarball, the one
    # file with an SBOM that is not a package (it lists the third-party code it holds).
    if set(packages) - set(compiled) or sorted(set(compiled) - set(packages)) != [chain.tarball_name("9.9.9")]:
        problems.append("build/ci/release-chain.py's compiled_assets is %s, and the release carries the compiled files %s "
                        "and the source tarball: each of them needs its SBOM" % (compiled, packages))
    for name in compiled:
        if chain.sbom_name(name) not in carried:
            problems.append("the release carries %s without its SBOM, %s (OpenSSF Baseline OSPS-QA-02.02; build/asset-sbom.py)" % (
                name, chain.sbom_name(name)))
    later = chain.made_after_collect("9.9.9")
    if chain.sbom_name(feed.installer_names("9.9.9")["windows"]) not in later:
        problems.append("build/ci/release-chain.py collects the installer's SBOM, which must be made from the bytes Authenticode "
                        "signs after collect")
    sbom_job = jobs["release-sbom"]
    if "python3 build/ci/release-chain.py sbom" not in sbom_job or not re.search(r"^      - release-sbom/\s*$", sbom_job, re.MULTILINE):
        problems.append("release-sbom does not run build/ci/release-chain.py sbom and keep release-sbom/")
    if re.search(r"^  id_tokens:", sbom_job, re.MULTILINE):
        problems.append("release-sbom holds an ID token: it signs nothing and needs none")
    if "sbom/native-dependencies-x64.json release-tools/" not in jobs["release"]:
        problems.append("release does not keep linux-sbom's component list in release-tools/, which release-sbom reads")
    for where, text in (("release", jobs["release"]), ("release-rehearsal", blocks(texts["rehearsal"]).get("release-rehearsal", ""))):
        if not re.search(r"^    - mkdir -p release-tools && cp [^\n]*sbom/native-dependencies-arm64\.json[^\n]* release-tools/\s*$", text, re.MULTILINE):
            problems.append("%s does not keep linux-sbom's ARM64 component list, sbom/native-dependencies-arm64.json, in "
                            "release-tools/, which release-sbom makes the ARM64 installer's SBOM from" % where)
    for needle, why in (('sboms=release-sbom', "release-sbom's artifact"),
                        ('cp "$sboms/$installer.cdx.json" "$out/$installer.cdx.json"', "the installer's SBOM among the files it signs"),
                        ('grep -q "\\"content\\": \\"$signed_sha256\\"" "$sboms/$installer.cdx.json"', "the check that it describes the signed installer")):
        if needle not in texts["sign"]:
            problems.append("build/ci/release-sign.sh no longer has %s (%s)" % (needle, why))
    if "python3 build/asset-sbom.py verify \"$file.cdx.json\" \"$file\"" not in texts["verify"]:
        problems.append("build/ci/release-verify.sh no longer holds each compiled file's SBOM to the file (build/asset-sbom.py verify)")
    for needle, why in (('said=$(python3 build/ci/sigstore-certificate.py repository-id "$signed/$installer.cosign.bundle")',
                         "the installer's repository id read from its certificate's extensions"),
                        ("  id=86730559) id_row=", "the id this project's")):
        if needle not in texts["verify"]:
            problems.append("build/ci/release-verify.sh no longer has %s (%s), which build/ci/sigstore-certificate.py reads "
                            "from the certificate's extensions and not from its bytes" % (needle, why))
    if "sbom_problems(assets, version)" not in function_text(texts["chain"], "publish"):
        problems.append("build/ci/release-chain.py's publish no longer holds each compiled file's SBOM to its file before it uploads")

    # Sigstore.
    sign = jobs["release-sign"]
    if not re.search(r"^  id_tokens:\s*\n\s+SIGSTORE_ID_TOKEN:\s*\n\s+aud: sigstore\s*$", sign, re.MULTILINE):
        problems.append("release-sign has no id_tokens SIGSTORE_ID_TOKEN with aud sigstore: Fulcio issues no certificate without it")
    if not re.search(r"^    name: \$HM_COSIGN_IMAGE\s*$", sign, re.MULTILINE):
        problems.append("release-sign's image is not $HM_COSIGN_IMAGE")
    if "build/ci/release-sign.sh" not in sign:
        problems.append("release-sign does not run build/ci/release-sign.sh")
    image = COSIGN_IMAGE.search(jobs.get("variables", ""))
    if not image:
        problems.append("the CI file's variables define no HM_COSIGN_IMAGE")
    elif PLACEHOLDER_COSIGN.match(image.group(1)):
        problems.append("HM_COSIGN_IMAGE is the placeholder %s again: it was pinned on 26 September 2026, and a release job "
                        "refuses a tag without its digest (docs/ContinuousIntegration.md, 'What waits for the owner')" % image.group(1))
    elif not PINNED_COSIGN.match(image.group(1)):
        problems.append("HM_COSIGN_IMAGE is %s: cosign comes from gcr.io/projectsigstore/cosign pinned by digest, never ghcr.io or GitHub" % image.group(1))
    # A project or pipeline CI/CD variable of the same name replaces the file's value
    # at run time, so the release job holds the value it is given to the same rule.
    if chain.PINNED_IMAGE.pattern != PINNED_COSIGN.pattern:
        problems.append("build/ci/release-chain.py holds HM_COSIGN_IMAGE to %s at run time, not to this checker's %s: a CI/CD "
                        "variable could put another registry's image in release-sign" % (chain.PINNED_IMAGE.pattern, PINNED_COSIGN.pattern))

    script = texts["sign"]
    flags = re.search(r'^sign_flags="([^"]*)"$', script, re.MULTILINE)
    flags = flags.group(1) if flags else ""
    for needle, why in (("--new-bundle-format", "the Sigstore bundle format, not cosign's older one"),
                        ("--rekor-url https://rekor.sigstore.dev", "the public Rekor v1 log, whose key servers compile in")):
        if needle not in flags:
            problems.append("build/ci/release-sign.sh signs without %s (%s)" % (needle, why))
    for needle, why in (('sign_flags="$sign_flags --use-signing-config=false"', "the signing configuration off, which could choose Rekor v2"),
                        ('.cosign.bundle"', "<file>.cosign.bundle, the name a server looks for"),
                        ('"hashedrekord"', "the check that the entry is a hashedrekord"),
                        ('0\\.0\\.1', "the check that the entry is version 0.0.1"),
                        ('"integratedTime"', "the check for the log's integratedTime")):
        if needle not in script:
            problems.append("build/ci/release-sign.sh no longer has %s (%s)" % (needle, why))
    counted = re.search(r"^expected=(\d+)$", script, re.MULTILINE)
    carried = len(chain.expected_assets("9.9.9"))
    if not counted or int(counted.group(1)) != carried:
        problems.append("build/ci/release-sign.sh counts %s file(s); a release carries %d (build/ci/release-chain.py)" % (
            counted.group(1) if counted else "no", carried))
    verified = re.search(r'\[ "\$verified" -eq (\d+) \]', texts["verify"])
    if not verified or int(verified.group(1)) != carried:
        problems.append("build/ci/release-verify.sh counts %s file(s); a release carries %d" % (verified.group(1) if verified else "no", carried))

    # The identity, the same in every place that names it.
    prefix = re.search(r'const char \*GITLAB_IDENTITY_PREFIX = "([^"]+)";', texts["verifier"])
    repository = re.search(r'const char \*GITLAB_REPOSITORY = "([^"]+)";', texts["verifier"])
    repository_id = re.search(r'const char \*GITLAB_REPOSITORY_ID = "([^"]+)";', texts["verifier"])
    if feed.PROJECT_URL != GROUP_PATH:
        problems.append("build/generate-update-feed.py's PROJECT_URL is %s, not the project's path %s" % (feed.PROJECT_URL, GROUP_PATH))
    # The path the project began at: the pipeline there signed 6.3.5, and a server must
    # go on taking that release (to install it, or to roll back to it).
    expected_prefix = FIRST_PATH + IDENTITY_TAIL + "v"
    if not prefix or prefix.group(1) != expected_prefix:
        problems.append("SigstoreVerifier.cpp's GITLAB_IDENTITY_PREFIX is %s, not %s - the path the project began at, which signed "
                        "6.3.5, and this CI file" % (prefix.group(1) if prefix else "missing", expected_prefix))
    # The group's path, the project's since 29 September 2026: the same pipeline, the
    # same project's id, and that id required - were it optional, a certificate for the
    # path would be accepted from whoever held the path, were the group ever lost.
    group_prefix = re.search(r'const char \*GITLAB_GROUP_IDENTITY_PREFIX = "([^"]+)";', texts["verifier"])
    group_repository = re.search(r'const char \*GITLAB_GROUP_REPOSITORY = "([^"]+)";', texts["verifier"])
    expected_group_prefix = GROUP_PATH + IDENTITY_TAIL + "v"
    if not group_prefix or group_prefix.group(1) != expected_group_prefix:
        problems.append("SigstoreVerifier.cpp's GITLAB_GROUP_IDENTITY_PREFIX is %s, not %s - this project's path and this CI file" % (
            group_prefix.group(1) if group_prefix else "missing", expected_group_prefix))
    if chain.SIGNER_IDENTITY_PREFIX + "v" != expected_group_prefix:
        problems.append("build/ci/release-chain.py expects the identity %s..., not the verifier's %s" % (chain.SIGNER_IDENTITY_PREFIX, expected_group_prefix))
    if not group_repository or group_repository.group(1) != GROUP_PATH:
        problems.append("SigstoreVerifier.cpp's GITLAB_GROUP_REPOSITORY is not %s" % GROUP_PATH)
    # The first path's id is required too, from 6.3.6: 6.3.5's certificate, the one
    # release signed there, records it, and a certificate for the path without it is
    # from a project that is not this one - the path is in a personal namespace the
    # project has left, which passes to someone else when its owner is renamed.
    first_signer = 'trust.signers.push_back(MakeSigner_(GITLAB_IDENTITY_PREFIX, GITLAB_ISSUER, GITLAB_REPOSITORY, GITLAB_REPOSITORY_ID, false, 0, ""));'
    if first_signer not in texts["verifier"] or texts["verifier"].count("MakeSigner_(GITLAB_IDENTITY_PREFIX") != 1:
        problems.append("SigstoreVerifier.cpp's DefaultTrust no longer trusts the GitLab signer at the path the project began at, "
                        "which signed 6.3.5, once, with the project's id required: %s" % first_signer)
    group_signer = 'trust.signers.push_back(MakeSigner_(GITLAB_GROUP_IDENTITY_PREFIX, GITLAB_ISSUER, GITLAB_GROUP_REPOSITORY, GITLAB_REPOSITORY_ID, false, 0, ""));'
    if group_signer not in texts["verifier"] or texts["verifier"].count("MakeSigner_(GITLAB_GROUP_IDENTITY_PREFIX") != 1:
        problems.append("SigstoreVerifier.cpp's DefaultTrust no longer trusts the group's signer once, with the project's id required: %s" % group_signer)
    # And whatever the settings make of the signers, a certificate naming either of the
    # project's GitLab paths or repositories needs the project's id: the check in
    # Verify_ that every signer's match goes through, over both paths.
    for needle in ("const char *const projectPaths[] = { GITLAB_REPOSITORY, GITLAB_GROUP_REPOSITORY };",
                   'const std::string path = LowerAscii_(std::string(projectPath) + "//");',
                   "if (repositoryRecorded && LowerAscii_(repository) == LowerAscii_(projectPath))",
                   'repositoryId != std::string(GITLAB_REPOSITORY_ID)',
                   "         else if (!projectRefusal.IsEmpty())\n            why = projectRefusal;\n"):
        if needle not in texts["verifier"]:
            problems.append("SigstoreVerifier.cpp no longer requires the project's id of every certificate naming one of the "
                            "project's GitLab paths or repositories, whatever the settings (%s)" % needle.strip())
    # The scripts name the identity after the project's path, which must be the group's,
    # with its own repository, and nothing else - not the first path, where a pipeline
    # now is another project's: the case on CI_PROJECT_PATH exactly as below, the one
    # assignment of the repository and the identity in each script, and the identity
    # right after the case.
    paths = [(GROUP_PATH.split("/", 3)[3], GROUP_PATH)]
    case_block = ('case "${CI_PROJECT_PATH:-}" in\n' +
                  "".join("  %s) repository=%s ;;\n" % path for path in paths) +
                  "  *) fail \"the project's path is '${CI_PROJECT_PATH:-}', not one a server trusts a signature from\" ;;\n"
                  "esac\n")
    for name in ("sign", "verify"):
        text = texts[name]
        if (text.count(case_block) != 1 or len(re.findall(r"\bcase\b[^\n]*CI_PROJECT_PATH", text)) != 1 or
                len(re.findall(r"(?<![\w-])repository=", text)) != len(paths)):
            problems.append("%s accepts the paths it names otherwise than %s alone, each with its repository, and "
                            "fails at any other: its case on CI_PROJECT_PATH must be exactly\n%s" % (FILES[name], [p for p, _ in paths], case_block))
        if text.count(case_block + SCRIPT_IDENTITY + "\n") != 1 or len(re.findall(r"(?<![\w-])identity=", text)) != 1:
            problems.append("%s expects another identity than <the project's repository>%s<tag>, named once, after the case" % (FILES[name], IDENTITY_TAIL))
    # The ref the identity names: the tag's, set where the tag was held to v*, and the
    # branch's only after --rehearse, with the branch held to a protected batch* one
    # and no tag; and the paths release-chain.py signs its provenance for, the same.
    guards = {"sign": ('[ -z "$tag" ] || fail', "    batch*) ;;", '[ "${CI_COMMIT_REF_PROTECTED:-}" = true ] || fail'),
              "verify": ('[ -z "$tag" ] || fail', "    batch*) ;;")}
    for name in ("sign", "verify"):
        text = texts[name]
        tag_at, branch_at = text.find(TAG_REF), text.find(BRANCH_REF)
        rehearse_at = text.find("  --rehearse) rehearse=1 ;;")
        if text.count(TAG_REF) != 1 or text.count(BRANCH_REF) != 1 or len(re.findall(r"(?<![\w-])ref=", text)) != 2:
            problems.append("%s names the ref it signs as otherwise than %s once and %s once" % (FILES[name], TAG_REF, BRANCH_REF))
        elif "    v[0-9]*) ;;" not in text[max(0, tag_at - 300):tag_at]:
            problems.append("%s signs a release as %s without holding the tag to v*" % (FILES[name], TAG_REF))
        elif rehearse_at < 0 or not rehearse_at < branch_at or not all(g in text[rehearse_at:branch_at] for g in guards[name]) \
                or text.count("rehearse=1") != 1:
            problems.append("%s signs as a branch (%s) outside --rehearse, or without holding the branch to a protected batch* "
                            "one and the pipeline to one without a tag: %s" % (FILES[name], BRANCH_REF, " ".join(guards[name])))
    if chain.SIGNING_REPOSITORIES != dict(paths):
        problems.append("build/ci/release-chain.py signs its provenance for %s, not the paths the scripts sign from, %s" % (
            chain.SIGNING_REPOSITORIES, dict(paths)))
    if not repository or repository.group(1) != FIRST_PATH:
        problems.append("SigstoreVerifier.cpp's GITLAB_REPOSITORY is not %s" % FIRST_PATH)
    if not repository_id or repository_id.group(1) != feed.PROJECT_ID or chain.PROJECT_ID != feed.PROJECT_ID:
        problems.append("SigstoreVerifier.cpp's GITLAB_REPOSITORY_ID is not the project id %s" % feed.PROJECT_ID)
    if not os.path.isfile(os.path.join(ROOT, ".gitlab-ci.yml")):
        problems.append("the CI file is not .gitlab-ci.yml at the root, the file every GitLab certificate names")

    # The release tag: master's allow-list, of one key, and the tagged commit on master.
    keys = chain.signer_keys(texts["signers"])
    if len(keys) != 1 or not chain.fingerprint(keys[0][2]):
        problems.append(".gitlab/allowed_signers lists %d key(s): release tags are the owner's alone (GOVERNANCE.md), and the "
                        "release job refuses every tag while master's list holds any number but one" % len(keys))
    if chain.TRUSTED_BRANCH != "master" or chain.TRUSTED_REF != "refs/remotes/origin/master":
        problems.append("build/ci/release-chain.py reads the allow-list and the tag's ancestry from %s (%s), not from master, "
                        "the protected branch" % (chain.TRUSTED_BRANCH, chain.TRUSTED_REF))
    problems.extend(probe_tag_check(chain))

    # The registry: collect refuses what a release cannot be made from; publish refuses
    # a name held with other bytes before it sends anything, does not send again what
    # is up with the same bytes, and holds every record of each name to its digest.
    problems.extend(probe_registry(chain, feed))
    collect_text = function_text(texts["chain"], "collect")
    if "check_registry_before_signing(api, version," not in collect_text:
        problems.append("build/ci/release-chain.py's collect no longer looks at the registry before anything is signed")
    if "api=GitLab(token) if token else None" not in function_text(texts["chain"], "main"):
        problems.append("build/ci/release-chain.py's main no longer gives collect the job's token to read the registry with")
    publish_text = function_text(texts["chain"], "publish")
    looked = publish_text.find("conflicts, same = registry_conflicts(registry_files(api, version), digests, version)")
    refused = publish_text.find("    if conflicts:\n        raise ReleaseError")
    sent = publish_text.find('api.request("PUT"')
    if looked < 0 or refused < looked or sent < refused:
        problems.append("build/ci/release-chain.py's publish no longer refuses a name the registry holds with other bytes "
                        "before it uploads anything")
    if "        if name in same:\n" not in publish_text or sent < publish_text.find("        if name in same:\n"):
        problems.append("build/ci/release-chain.py's publish sends again a file the registry holds with the same bytes")
    if not re.search(r"^        for entry in entries:\n            if \(entry\.get\(\"file_sha256\"\)", publish_text[max(sent, 0):], re.MULTILINE):
        problems.append("build/ci/release-chain.py's publish no longer holds every record of each name to its digest after "
                        "the upload: a download by name is served the newest, and another may be older")

    # Authenticode.
    authenticode = jobs["release-authenticode"]
    if not re.search(r"^  id_tokens:\s*\n\s+AZURE_ID_TOKEN:\s*\n\s+aud: api://AzureADTokenExchange\s*$", authenticode, re.MULTILINE):
        problems.append("release-authenticode has no id_tokens AZURE_ID_TOKEN with aud api://AzureADTokenExchange, which Entra ID requires")
    if "tags: [bench150]" not in authenticode or "build\\ci\\windows-authenticode.ps1" not in authenticode:
        problems.append("release-authenticode does not run build\\ci\\windows-authenticode.ps1 on bench150")
    ps1 = texts["authenticode"]
    if "$timestampUrl = 'http://timestamp.acs.microsoft.com'" not in ps1 or "/tr $timestampUrl /td SHA256" not in ps1:
        problems.append("build/ci/windows-authenticode.ps1 signs without the service's RFC 3161 timestamp; the certificate lives three days")
    if "TimeStamperCertificate" not in ps1:
        problems.append("build/ci/windows-authenticode.ps1 no longer refuses a signature without a timestamp")
    listed = re.search(r"^\$settings = (.+)$", ps1, re.MULTILINE)
    names = re.findall(r"'([A-Z_]+)'", listed.group(1)) if listed else []
    if names != chain.AZURE_SETTINGS:
        problems.append("build/ci/windows-authenticode.ps1 reads %s; the release job checks %s" % (names, chain.AZURE_SETTINGS))
    installer = feed.installer_names("$version")["windows"]
    for name in ("authenticode", "installer"):
        text = jobs["release-authenticode"] if name == "authenticode" else texts["installer"]
        if installer not in text:
            problems.append("%s does not name the installer %s, the name a Windows server looks for" % (
                "release-authenticode" if name == "authenticode" else FILES["installer"], installer))

    # The ARM64 installer: built by windows-packages-arm64, which release needs and which
    # may not fail or wait in a tag's pipeline; collected from where it leaves it, held
    # to its sums; signed by its own call into its own folder, which release-sbom and
    # release-sign.sh read; and taken by release-sign.sh with its SBOM.
    arm64 = feed.installer_names("$version")["windows-arm64"]
    windows_jobs = blocks(texts["packages-windows"])
    if ARM64_INSTALLER_JOB not in windows_jobs:
        problems.append("%s has no job %s, which builds %s, a file the release carries" % (
            FILES["packages-windows"], ARM64_INSTALLER_JOB, feed.installer_names("9.9.9")["windows-arm64"]))
    else:
        entry = need_entries(jobs["release"]).get(ARM64_INSTALLER_JOB)
        if entry is not None and re.search(r"optional: true", entry):
            problems.append("release needs %s optionally: a tag's pipeline without it would release past the ARM64 installer" % ARM64_INSTALLER_JOB)
        for escape in tag_escapes(ARM64_INSTALLER_JOB, dict(jobs, **windows_jobs)):
            problems.append("%s can let release start in a v* tag's pipeline without its having passed (%s): release would "
                            "carry %s past it" % (ARM64_INSTALLER_JOB, escape, feed.installer_names("9.9.9")["windows-arm64"]))
        # A tag's x64 installer is windows-installer's alone (Roadmap3, "The x64 installer
        # made and signed by the pipeline"): windows-packages-x64 refuses a tag first, and
        # the ARM64 job, which follows it for the gate lock on a branch, needs it optionally.
        x64_entry = need_entries(windows_jobs[ARM64_INSTALLER_JOB]).get(BRANCH_X64_INSTALLER_JOB)
        if x64_entry is not None and not re.search(r"optional: true", x64_entry):
            problems.append("%s needs %s without optional: true, and a v* tag's pipeline, which has no %s, would not be "
                            "created" % (ARM64_INSTALLER_JOB, BRANCH_X64_INSTALLER_JOB, BRANCH_X64_INSTALLER_JOB))
    x64_job = windows_jobs.get(BRANCH_X64_INSTALLER_JOB, "")
    if x64_job and not re.search(r"^  rules:\s*\n    - if: \$CI_COMMIT_TAG\s*\n      when: never\s*\n", x64_job, re.MULTILINE):
        problems.append("%s does not refuse a v* tag first (rules: - if: $CI_COMMIT_TAG, when: never): a release pipeline "
                        "would build a second x64 installer under the gate lock, which it does not carry - windows-installer's "
                        "is the one" % BRANCH_X64_INSTALLER_JOB)
    where = chain.sources("9.9.9")
    arm64_name = feed.installer_names("9.9.9")["windows-arm64"]
    if (where.get(arm64_name) or "").replace(os.sep, "/") != ARM64_INSTALLER_DIR + arm64_name or \
            (chain.package_job_sums("9.9.9").get(arm64_name) or ("", ""))[0].replace(os.sep, "/") != ARM64_INSTALLER_DIR + "SHA256SUMS":
        problems.append("build/ci/release-chain.py does not collect %s from %s%s held to that job's %sSHA256SUMS" % (
            arm64_name, ARM64_INSTALLER_DIR, arm64_name, ARM64_INSTALLER_DIR))
    if arm64_name not in chain.compiled_assets("9.9.9") or arm64_name not in chain.made_after_collect("9.9.9") or \
            chain.sbom_name(arm64_name) not in chain.made_after_collect("9.9.9"):
        problems.append("build/ci/release-chain.py does not carry %s as a compiled file whose signed bytes and SBOM are made "
                        "after collect" % arm64_name)
    authenticode_call = '-Installer "release-assets\\%s" -OutDir %s' % (arm64, ARM64_SIGNED_DIR)
    if authenticode_call not in jobs["release-authenticode"] or \
            not re.search(r"^      - %s/\s*$" % re.escape(ARM64_SIGNED_DIR), jobs["release-authenticode"], re.MULTILINE):
        problems.append("release-authenticode does not sign the ARM64 installer (%s) or keep %s/: its SBOM and its "
                        "Sigstore signature would be made from the unsigned bytes" % (authenticode_call, ARM64_SIGNED_DIR))
    # The job's exit is its last call's: without the stop after the x64 call, an x64
    # installer that was not signed would end a job the ARM64 call made pass.
    x64_call = '-Installer "release-assets\\%s" -OutDir release-authenticode\n' % installer
    if x64_call + "      if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n" not in jobs["release-authenticode"]:
        problems.append("release-authenticode does not stop when the x64 installer's call fails (%sif ($LASTEXITCODE -ne 0) "
                        "{ exit $LASTEXITCODE }): the job would pass on the ARM64 call's exit alone" % x64_call.replace("\n", "; "))
    if chain.INSTALLER_SBOMS["windows-arm64"]["signed"] != ARM64_SIGNED_DIR:
        problems.append("build/ci/release-chain.py's sbom reads the signed ARM64 installer from %s, and release-authenticode "
                        "signs it into %s" % (chain.INSTALLER_SBOMS["windows-arm64"]["signed"], ARM64_SIGNED_DIR))
    # Each needle as often as the script needs it: the loop over both installers twice,
    # once to hold each to its SBOM and once to take it into what is signed.
    for needle, times, why in (('installer_arm64="hMailServer-${version}-arm64.exe"', 1, "the ARM64 installer's name"),
                               ('for installer in "$installer_x64" "$installer_arm64"; do', 2,
                                "both installers taken, held to their SBOMs and copied"),
                               ('      *-arm64.exe) echo "$authenticode-arm64/$1" ;;', 1,
                                "the ARM64 installer taken signed, from release-authenticode-arm64/")):
        if texts["sign"].count(needle) != times:
            problems.append("build/ci/release-sign.sh no longer has %s %s (%s)" % (needle, "once" if times == 1 else "%d times" % times, why))

    # The PowerShell module (Roadmap3, "The PowerShell Gallery"): its two files carried by
    # every release as primary files without an SBOM, collected unsigned from the tree and
    # signed after collect; signed by release-authenticode's own call, made only when the
    # ARM64 one signed, into the folder release-chain.py's sbom and release-sign.sh read;
    # and taken from there by release-sign.sh.
    module = list(getattr(chain, "MODULE_FILES", ()))
    if module != ["HmailServer.psd1", "HmailServer.psm1"] or any(
            n not in chain.primary_assets("9.9.9") or n in chain.compiled_assets("9.9.9") or n not in chain.made_after_collect("9.9.9")
            or (chain.sources("9.9.9").get(n) or "").replace(os.sep, "/") != "hmailserver/source/Tools/Cli/" + n for n in module):
        problems.append("build/ci/release-chain.py does not carry the PowerShell module's HmailServer.psd1 and HmailServer.psm1 as "
                        "primary files, collected from hmailserver/source/Tools/Cli and signed after collect")
    module_call = "-File build\\ci\\windows-authenticode.ps1 -Module release-assets -OutDir %s\n" % MODULE_SIGNED_DIR
    if module_call not in authenticode or not re.search(r"^      - %s/\s*$" % re.escape(MODULE_SIGNED_DIR), authenticode, re.MULTILINE) \
            or getattr(chain, "MODULE_SIGNED_DIR", None) != MODULE_SIGNED_DIR:
        problems.append("release-authenticode does not sign the PowerShell module (%s) and keep %s/, where release-chain.py's "
                        "sbom reads it: the Gallery and the release would carry it unsigned" % (module_call.strip(), MODULE_SIGNED_DIR))
    arm64_call = '-Installer "release-assets\\%s" -OutDir %s\n' % (arm64, ARM64_SIGNED_DIR)
    if arm64_call + "      if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n" not in authenticode:
        problems.append("release-authenticode does not stop when the ARM64 installer's call fails (%sif ($LASTEXITCODE -ne 0) "
                        "{ exit $LASTEXITCODE }): the job would pass on the module call's exit alone" % arm64_call.replace("\n", "; "))
    for needle, why in (('module_files="HmailServer.psd1 HmailServer.psm1"', "the module's two files"),
                        ('      HmailServer.psd1|HmailServer.psm1) echo "$authenticode-module/$1" ;;', "the module taken signed, from "
                         "release-authenticode-module/")):
        if texts["sign"].count(needle) != 1:
            problems.append("build/ci/release-sign.sh no longer has %s once (%s)" % (needle, why))
    if "function Get-BlockProblems(" not in ps1 or "$problems += @(Get-BlockProblems " not in ps1 or "-Module" not in ps1:
        problems.append("build/ci/windows-authenticode.ps1 no longer signs the module (-Module) holding each file to its unsigned "
                        "bytes and one signature block (Get-BlockProblems)")

    # The build inputs' signatures (Roadmap3, "Every committed binary checked against its
    # manifest in CI"): a job on bench150, on protected refs alone, that every build
    # placing them needs, and that cannot let one start without having passed.
    inputs_job = windows_jobs.get(BUILD_INPUTS_JOB)
    if inputs_job is None:
        problems.append("%s has no job %s, which checks the build inputs' Authenticode signatures before any build uses them" % (
            FILES["packages-windows"], BUILD_INPUTS_JOB))
    else:
        if not re.search(r"^  tags: \[bench150\]\s*$", inputs_job, re.MULTILINE) or not re.search(r"^  extends: \.own-runners\s*$", inputs_job, re.MULTILINE):
            problems.append("%s does not run on bench150 under .own-runners' rules (protected refs, never a merge request)" % BUILD_INPUTS_JOB)
        for script in ("build\\ci\\build-inputs-authenticode.ps1", "build\\ci\\authenticode-selftest.ps1"):
            if "-File %s\n" % script not in inputs_job:
                problems.append("%s does not run %s" % (BUILD_INPUTS_JOB, script))
        if "build-inputs-authenticode.ps1\n      if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n" not in inputs_job:
            problems.append("%s does not stop when the build inputs' check fails: the self-test's exit alone would end it" % BUILD_INPUTS_JOB)
        for escape in tag_escapes(BUILD_INPUTS_JOB, dict(jobs, **windows_jobs)):
            problems.append("%s can let a build start without its having passed (%s)" % (BUILD_INPUTS_JOB, escape))
        for builder, builder_text in ((".windows-packages", windows_jobs.get(".windows-packages", "")), ("windows-installer", jobs["windows-installer"])):
            entry = need_entries(builder_text).get(BUILD_INPUTS_JOB) if builder_text else None
            if BUILD_INPUTS_JOB not in (needs(builder_text) if builder_text else []) or (
                    entry is not None and re.search(r"optional: true", entry)):
                problems.append("%s does not need %s: it would build from inputs whose signatures were not checked" % (builder, BUILD_INPUTS_JOB))

    # The container image, in a file of its own that .gitlab-ci.yml includes. Its
    # !reference [.release-rules, rules] names the CI file's template.
    if not re.search(r"^  - local: \.gitlab/ci/container-image\.yml\s*$", ci, re.MULTILINE):
        problems.append(".gitlab-ci.yml does not include .gitlab/ci/container-image.yml: no release pushes its container image")
    image_jobs = blocks(texts["image"])
    merged = dict(jobs)
    merged.update(image_jobs)
    for name, wanted in sorted(IMAGE_JOBS.items()):
        if name not in image_jobs:
            problems.append(".gitlab/ci/container-image.yml has no %s job" % name)
            continue
        if TAG_ONLY not in rules_text(name, merged):
            problems.append("%s can run for something that is not a protected v* tag: its rules must be .release-rules" % name)
        if any("$HM_PUBLISH_IMAGE" in item for item in rule_items(image_jobs[name], merged)):
            problems.append("%s reads $HM_PUBLISH_IMAGE in its rules, which are decided when the pipeline is made: a release "
                            "published before the project's move would have no image jobs to run after it. The scripts read "
                            "the switch as the jobs run" % name)
        if re.search(r"^  trigger:", image_jobs[name], re.MULTILINE):
            problems.append("%s is a trigger: a child or downstream pipeline's ID token has no ci_config_ref_uri" % name)
        have = needs(image_jobs[name])
        for need in wanted:
            if need not in have:
                problems.append("%s does not need %s (it needs %s)" % (name, need, ", ".join(have) or "nothing"))
    for key in ("ci", "image", "security"):
        if re.search(r"^\s*HM_PUBLISH_IMAGE\s*:", texts[key], re.MULTILINE):
            problems.append("%s sets HM_PUBLISH_IMAGE: the container image's gate is the project's CI/CD variable, "
                            "which nothing committed may turn on" % FILES[key])
    # The first job waits for a person, and the two scripts that fetch and push read the
    # switch as they run, refusing with a non-zero exit.
    first = image_jobs.get(IMAGE_FIRST, "")
    if first:
        tag_rule = [item for item in rule_items(first, merged) if TAG_ONLY in item]
        if not tag_rule or not re.search(r"^\s*when: manual\s*$", tag_rule[0], re.MULTILINE):
            problems.append("%s is not started by hand in a v* tag's pipeline (when: manual): the image would be pushed "
                            "as soon as the release is published, whatever the maintainers meant" % IMAGE_FIRST)
        # A job that needs one allowed to fail starts after it has failed: the image would be
        # built after a refusal, or after a bundle that did not verify.
        if (tag_rule and not re.search(r"^\s*allow_failure: false\s*$", tag_rule[0], re.MULTILINE)) or \
                re.search(r"^  allow_failure:", first, re.MULTILINE):
            problems.append("%s may fail without stopping the jobs that need it (its tag rule must say allow_failure: false): "
                            "container-image would start after a refusal or a signature that did not verify" % IMAGE_FIRST)
        if not re.search(r"^    name: \$HM_COSIGN_IMAGE\s*$", first, re.MULTILINE) or "sh build/ci/container-image-deb.sh" not in first:
            problems.append("%s does not run build/ci/container-image-deb.sh in $HM_COSIGN_IMAGE, where cosign verifies "
                            "what it takes from the release" % IMAGE_FIRST)
    for key in GATED_SCRIPTS:
        # The exit inside the gate's own if, before its fi.
        gate = re.search(re.escape(RUNTIME_GATE) + r"\n((?:(?![ \t]*fi[ \t]*\n)[^\n]*\n){0,8}?)[ \t]*exit ([0-9]+)[ \t]*\n", texts[key])
        if not gate or gate.group(2) == "0":
            problems.append("%s does not refuse, exiting non-zero, unless the project's variable HM_PUBLISH_IMAGE is 1 "
                            "(%s ... exit <n>)" % (FILES[key], RUNTIME_GATE))
    # The push goes to the group's path alone: a switch set before the project's move
    # would otherwise push to the first path, an address the image would lose with it.
    # linux-image.sh builds a release's image for nowhere else, and container-image-push.sh
    # pushes nowhere else.
    for key in ("image-build", "image-push", "image-artifacthub"):
        if not re.search(r"^[ \t]+registry\.gitlab\.com/hmailserver/hmailserver\) ;;$", texts[key], re.MULTILINE) or \
                re.search(r"^[ \t]*[^#\n]*registry\.gitlab\.com/progressiverobot/hmailserver[^\n]*\)[ \t]*;;", texts[key], re.MULTILINE):
            problems.append("%s does not push to registry.gitlab.com/hmailserver/hmailserver alone: "
                            "it would push from the project's first path, whose image changes its address with the move" % FILES[key])
    fetch = texts["image-deb"]
    for needle, why in (('cosign verify-blob $verify_flags --bundle "$out/$name.cosign.bundle"', "each file verified against its bundle"),
                        ("  || fail \"this cosign has no verify-blob --new-bundle-format", "the bundle read strictly, as the "
                         "shell reader's reading of the certificate cosign verified needs: never the old format's cert"),
                        ('verify_flags="--new-bundle-format"', "the bundle read strictly"),
                        ('--certificate-identity "$candidate" --certificate-oidc-issuer "$issuer"', "as this project's pipeline"),
                        ('candidate="$repository//.gitlab-ci.yml@refs/tags/$tag"', "for this tag"),
                        ('said=$(sh build/ci/sigstore-certificate.sh repository-id "$out/$name.cosign.bundle")',
                         "the project's id read from the certificate's extensions"),
                        ('"id=$project_id") id_said="$project_id" ;;', "the project's id in the certificate"),
                        ('      absent) fail "the certificate of $name records no repository id" ;;', "the id required at either path"),
                        ('for name in "$deb" "$deb.cdx.json"; do', "the .deb and its SBOM"),
                        ('for arch in %s; do' % " ".join(IMAGE_PLATFORMS), "each platform's .deb"),
                        ('deb="hmailserver_${version}_${arch}.deb"', "the .deb of each architecture"),
                        ('"$api/packages/generic/hmailserver/$version/$name"', "from the release's own files"),
                        ('sums="hmailserver-linux-$version-SHA256SUMS.txt"\ntake "$sums"\n',
                         "the release's signed sums taken and verified as the rest"),
                        ('      { grep -Fxq "$deb_sum  $deb" "$out/$sums" || grep -Fxq "$deb_sum *$deb" "$out/$sums"; } \\\n'
                         '        || fail', "each .deb held to the SHA-256 the release's signed sums give its name"),
                        ('      grep -Fq "$deb_sum" "$out/$name" \\\n        || fail',
                         "each SBOM held to naming its .deb's SHA-256")):
        if needle not in fetch:
            problems.append("build/ci/container-image-deb.sh no longer has %s (%s)" % (needle, why))
    # busybox's wget checks no server certificate: nothing it sends may be a credential.
    for number, line in enumerate(fetch.split("\n"), 1):
        if not line.lstrip().startswith("#") and re.search(r"TOKEN|PASSWORD|--header|Authorization", line, re.IGNORECASE):
            problems.append("build/ci/container-image-deb.sh line %d sends or names a credential, over a TLS its wget does not "
                            "check: %s" % (number, line.strip()))
    problems += image_platform_problems(texts, jobs, image_jobs)
    problems += artifacthub_problems(texts, merged, image_jobs)
    found, said = image_tag_problems(texts)
    problems += found
    notes += said
    for name in ["container-image-" + arch for arch in IMAGE_PLATFORMS] + ["container-image-sbom"]:
        if name in image_jobs and not re.search(r"^    HM_IMAGE_DEB_DIR: container-image-deb\s*$", image_jobs[name], re.MULTILINE):
            problems.append("%s does not take the .deb from container-image-deb's artifact (HM_IMAGE_DEB_DIR: container-image-deb), "
                            "the bytes taken from the published release and verified" % name)
    start = image_jobs.get("container-image-start", "")
    if start:
        command = re.search(r"^START_COMMAND='([^'\n]+)'$", texts["image-start"], re.MULTILINE)
        if not command:
            problems.append("build/ci/container-image-start-test.sh names no START_COMMAND")
        elif '      command: ["bash", "-c", "%s"]' % command.group(1) not in start:
            problems.append("container-image-start's image service does not run build/ci/container-image-start-test.sh's "
                            "START_COMMAND, so the job and the test by hand start the image differently")
        if not re.search(r"^    - name: \$HM_IMAGE\s*\n      alias: mail\s*$", start, re.MULTILINE):
            problems.append("container-image-start does not start the pushed image by its digest ($HM_IMAGE) as the service mail")
        if not re.search(r'^    FF_NETWORK_PER_BUILD: "true"\s*$', start, re.MULTILINE):
            problems.append("container-image-start has no network of its own (FF_NETWORK_PER_BUILD): the image cannot "
                            "reach the database service by its alias")
        if "bash build/ci/container-image-start-test.sh" not in start:
            problems.append("container-image-start does not run build/ci/container-image-start-test.sh")
    image_sign = image_jobs.get("container-image-sign", "")
    if image_sign:
        if not re.search(r"^  id_tokens:\s*\n\s+SIGSTORE_ID_TOKEN:\s*\n\s+aud: sigstore\s*$", image_sign, re.MULTILINE):
            problems.append("container-image-sign has no id_tokens SIGSTORE_ID_TOKEN with aud sigstore: Fulcio issues no certificate without it")
        if not re.search(r"^    name: \$HM_COSIGN_IMAGE\s*$", image_sign, re.MULTILINE):
            problems.append("container-image-sign's image is not $HM_COSIGN_IMAGE")
        if "build/ci/container-image-sign.sh" not in image_sign:
            problems.append("container-image-sign does not run build/ci/container-image-sign.sh")
    scanning = image_jobs.get("container_scanning", "")
    if scanning and not re.search(r"^  dependencies: \[container-image\]\s*$", scanning, re.MULTILINE):
        problems.append("container_scanning does not take container-image's dotenv report (dependencies: [container-image]): "
                        "it would have no platform's image by its digest to scan")
    if scanning:
        scanner = CONTAINER_SCANNER.search(scanning)
        trivy = CONTAINER_SCANNER.search(jobs.get("dependency-scan", ""))
        if not scanner or not trivy or scanner.group(1) != trivy.group(1):
            problems.append("container_scanning does not run in dependency-scan's Trivy image by the same release and digest, "
                            "on its image's name: line (%s; dependency-scan's %s): its scanner is Trivy, pinned where Renovate "
                            "moves both, not GitLab's analyzer, which GitLab rebuilds daily and stops serving within days of "
                            "(ContinuousIntegration.md, \"container_scanning's scanner\")"
                            % (scanner.group(1) if scanner else "none", trivy.group(1) if trivy else "none"))
        for needle, why in (("    HM_SCAN_ARCH: amd64\n", "scan the linux/amd64 image (HM_SCAN_ARCH: amd64)"),
                            ("    - sh build/ci/container-scan.sh\n", "run build/ci/container-scan.sh"),
                            ("    reports:\n      container_scanning: container-scan/gl-container-scanning-report.json\n",
                             "keep the script's report as the job's container_scanning report")):
            if needle not in scanning + "\n":
                problems.append("container_scanning does not %s" % why)
    if re.search(r"^  (?:image|script|before_script|artifacts|allow_failure):", image_jobs.get("container_scanning_arm64", ""),
                 re.MULTILINE):
        problems.append("container_scanning_arm64 has an image, a script or artifacts of its own: it is container_scanning's "
                        "twin, the same scan of the linux/arm64 image, which it extends")
    every_ci = dict(texts.get("ci-includes", {}), **{".gitlab-ci.yml": ci, FILES["image"].replace(os.sep, "/"): texts["image"]})
    for path, text in sorted(every_ci.items()):
        named = [line.strip() for line in text.split("\n") if GITLAB_SCANNER.search(line) and not line.lstrip().startswith("#")]
        if named:
            problems.append("%s names GitLab's container-scanning analyzer or includes its template (%s): GitLab rebuilds "
                            "that image daily and stops serving a digest of it within days, and it may not be copied; "
                            "container_scanning runs Trivy (ContinuousIntegration.md, \"container_scanning's scanner\")"
                            % (path, named[0][:100]))
    for needle in CONTAINER_SCAN_SCRIPT + tuple("  %s) image=${HM_IMAGE_%s:-} ;;\n" % (arch, arch.upper())
                                                for arch in IMAGE_PLATFORMS):
        if needle not in texts["image-scan"]:
            problems.append("%s no longer has %s: it scans each platform's image by its digest, the project's alone, "
                            "and keeps the report in GitLab's format where the job takes it" % (FILES["image-scan"], needle.strip()))
    image_script = texts["image-sign"]
    for needle, why in (('platforms="%s"' % " ".join(IMAGE_PLATFORMS), "every platform's image signed"),
                        ('\nsign index "$index"\n', "the index, which every tag names, signed"),
                        ('  sign "$arch" "$image"\n', "each platform's image signed by its own digest"),
                        ('  sbom="container-image-sbom/hmailserver-image-$arch.cdx.json"\n  sign "$arch"', "each platform's own SBOM attested to it")):
        if needle not in image_script:
            problems.append("build/ci/container-image-sign.sh no longer has %s (%s)" % (needle.strip(), why))
    if TAG_SCRIPT_IDENTITY not in image_script:
        problems.append("build/ci/container-image-sign.sh expects another identity than %s" % TAG_SCRIPT_IDENTITY)
    group_case = "  %s) repository=%s ;;\n" % (GROUP_PATH.split("/", 3)[3], GROUP_PATH)
    if image_script.count(group_case) != 1 or len(re.findall(r"(?<![\w-])repository=", image_script)) != 1:
        problems.append("build/ci/container-image-sign.sh signs from another path than the group's, %s, alone" % GROUP_PATH)
    image_flags = re.search(r'^sign_flags="([^"]*)"$', image_script, re.MULTILINE)
    if not image_flags or "--rekor-url https://rekor.sigstore.dev" not in image_flags.group(1):
        problems.append("build/ci/container-image-sign.sh signs without --rekor-url https://rekor.sigstore.dev (Rekor v1)")
    if "--use-signing-config=false" not in image_script:
        problems.append("build/ci/container-image-sign.sh no longer turns the signing configuration off")
    for needle, why in (('cosign attest $attest_flags --type cyclonedx --predicate "$sbom" "$image"', "the SBOM attested to the image"),
                        ('cosign verify-attestation $verify_flags --type cyclonedx --certificate-identity "$identity"', "the attestation verified as the release's identity"),
                        ('sbom="container-image-sbom/hmailserver-image-$arch.cdx.json"', "the SBOM container-image-sbom made")):
        if needle not in image_script:
            problems.append("build/ci/container-image-sign.sh no longer has %s (%s): the image would go out without its SBOM" % (needle, why))
    # Each signature's certificate held to the project's id, which the identity, a path,
    # does not prove (ContainerImage.md, "Checking an image").
    for needle, why in (('cosign sign $sign_flags --output-certificate "$out/certificate-$1.pem" "$2"', "the certificate of each signature kept"),
                        ('\n  certificate_id "$1"\n}\n', "each signature's certificate read for the project's id"),
                        ('said=$(sh build/ci/sigstore-certificate.sh repository-id "$out/certificate-$1.json")', "the id read from the certificate's extensions"),
                        ('\n    "id=$project_id") ;;\n', "only the project's id taken"),
                        ('\nproject_id=86730559\n', "the project's id")):
        if needle not in image_script:
            problems.append("build/ci/container-image-sign.sh no longer has %s (%s): an image signed by another project at "
                            "the same path would pass its self-check" % (needle.strip(), why))
    image_sbom = image_jobs.get("container-image-sbom", "")
    if image_sbom and ("bash build/ci/container-image-sbom.sh" not in image_sbom or "- container-image-sbom/" not in image_sbom):
        problems.append("container-image-sbom does not run build/ci/container-image-sbom.sh and keep container-image-sbom/")

    # The .sigstore.json copies and the provenance: made in release-sign.sh, held to the
    # files in release-verify.sh, and before any upload in publish.
    sign_text, verify_text = texts["sign"], texts["verify"]
    for needle, why in (('cp "$bundle" "$file.sigstore.json"', "each primary file's bundle copied to <file>.sigstore.json"),
                        ('cmp -s "$bundle" "$file.sigstore.json"', "the copy held to its bundle"),
                        ('provenance="hmailserver-${version}.intoto.sigstore.json"', "the provenance's name, as release-chain.py's"),
                        ('statement="hmailserver-${version}.intoto.json"', "the statement's name, as release-chain.py's"),
                        ('cosign attest-blob $attest_flags --statement "$sboms/$statement" --bundle "$out/$provenance"',
                         "the statement attested as the pipeline"),
                        ('cosign verify-blob-attestation $verify_flags --bundle "$out/$provenance" --type slsaprovenance1',
                         "every file found in the provenance"),
                        ('--certificate-identity "$identity" --certificate-oidc-issuer "$issuer" "$file" > /dev/null 2>&1 \\\n    || fail "$(basename "$file") is not in the provenance',
                         "as the pipeline's identity"),
                        ('[ "$attested" -eq "$expected" ]', "every file counted in the provenance"),
                        ('grep -Eq \'"kind" *: *"dsse"\' "$out/$provenance"', "the provenance's Rekor v1 dsse entry")):
        if needle not in sign_text:
            problems.append("build/ci/release-sign.sh no longer has %s (%s)" % (needle.split("\n")[0], why))
    attest = re.search(r'^attest_flags="([^"]*)"$', sign_text, re.MULTILINE)
    if not attest or "--new-bundle-format" not in attest.group(1) or "--rekor-url https://rekor.sigstore.dev" not in attest.group(1) \
            or "attest_flags=\"$attest_flags --use-signing-config=false\"" not in sign_text:
        problems.append("build/ci/release-sign.sh attests the provenance without the bundle format, Rekor v1 at "
                        "rekor.sigstore.dev and the signing configuration off")
    if chain.provenance_name("${version}") != "hmailserver-${version}.intoto.sigstore.json" or \
            chain.statement_name("${version}") != "hmailserver-${version}.intoto.json":
        problems.append("build/ci/release-chain.py names the provenance %s and its statement %s, not what release-sign.sh names them" % (
            chain.provenance_name("${version}"), chain.statement_name("${version}")))
    for name, text in (("sign", sign_text), ("verify", verify_text)):
        if text.count("  case \"$file\" in *.cosign.bundle|*.sigstore.json) continue ;; esac") != (2 if name == "sign" else 1):
            problems.append("%s signs or checks a bundle or a .sigstore.json as a file of the release, or no longer passes "
                            "over them" % FILES[name])
    described = re.search(r'\[ "\$described" -eq (\d+) \]', verify_text)
    if not described or int(described.group(1)) != len(chain.compiled_assets("9.9.9")):
        problems.append("build/ci/release-verify.sh holds %s file(s) to their SBOM; a release has %d" % (
            described.group(1) if described else "no", len(chain.compiled_assets("9.9.9"))))
    globbed = re.search(r'^for file in ("\$signed"/\*\S+(?: "\$signed"/\*\S+)*); do$', verify_text, re.MULTILINE)
    globbed = set(re.findall(r'"\$signed"/\*(\S+)', globbed.group(1))) if globbed else set()
    for name in chain.compiled_assets("9.9.9"):
        if not any(name.endswith(suffix) for suffix in globbed):
            problems.append("build/ci/release-verify.sh does not hold %s to its SBOM: its loop takes %s" % (name, sorted(globbed)))
    # release-verify.sh checks the copies and the provenance for a rehearsal with
    # --rehearse and for a release without, in two calls, since one with an unquoted
    # ${rehearse:+--rehearse} is a semgrep finding the release's gate would refuse.
    rehearsal_call = ('if [ -n "$rehearse" ]; then\n  python3 build/ci/release-chain.py check-provenance --assets "$signed" '
                      '--rehearse \\\n    || fail ')
    release_call = 'else\n  python3 build/ci/release-chain.py check-provenance --assets "$signed" \\\n    || fail '
    if rehearsal_call not in verify_text or release_call not in verify_text:
        problems.append("build/ci/release-verify.sh no longer holds the .sigstore.json copies and the provenance to the files "
                        "(build/ci/release-chain.py check-provenance), for a rehearsal with --rehearse and for a release without")
    refusal = "The certificate names {0}, not this repository's release workflow."
    if refusal not in texts["verifier"] or '[ "$code" -ne 0 ] || fail' not in verify_text or \
            '*"The certificate names $identity, not this repository\'s release workflow."*) ;;' not in verify_text:
        problems.append("build/ci/release-verify.sh --rehearse no longer requires the server's verifier to refuse each "
                        "rehearsal bundle at its identity, with SigstoreVerifier.cpp's words (%s)" % refusal)
    publish_text = function_text(texts["chain"], "publish")
    first_put = publish_text.find('api.request("PUT"')
    for needle in ("copy_problems(assets, version)", "provenance_problems(", "link_problems(release_links(version, wanted), description)",
                   "check_budget(api, tag, version, digests"):
        at = publish_text.find(needle)
        if at < 0 or first_put < 0 or at > first_put:
            problems.append("build/ci/release-chain.py's publish uploads before it has held the release to %s" % needle)
    collect_text = function_text(texts["chain"], "collect")
    if collect_text.find("check_budget(") < 0 or collect_text.find("check_budget(") > collect_text.find("shutil.copyfile(") \
            or "tarball_problems(" not in collect_text:
        problems.append("build/ci/release-chain.py's collect no longer holds the tarball to build/check-source-tarball.py, or "
                        "counts what GitLab and Pages take only after it has copied the files")
    feed_max = re.search(r"const size_t MAX_FEED_BYTES = ([0-9 *]+);", texts["checker"])
    size = 1
    for factor in (feed_max.group(1).split("*") if feed_max else ["0"]):
        size *= int(factor.strip() or "0")
    if size != chain.MAX_FEED_BYTES:
        problems.append("build/ci/release-chain.py counts the feed against %d bytes, and a server reads %d (UpdateChecker.cpp, "
                        "MAX_FEED_BYTES)" % (chain.MAX_FEED_BYTES, size))
    problems.extend(link_path_problems(chain, texts.get("permalinks", {})))

    # Every other file the release carries from the package jobs, gathered by one job
    # that needs each build and its test, none of which may fail on a tag.
    job_file = {}
    everything = dict(jobs)
    for key in ("packages-deb", "packages-rpm", "packages-appimage", "tarball-ci", "release-packages", "helm", "packages-windows"):
        for name, text in blocks(texts[key]).items():
            if not name.startswith("."):
                job_file[name] = key
            everything.setdefault(name, text)
    gather = blocks(texts["release-packages"]).get(GATHER_JOB)
    if not re.search(r"^  - local: \.gitlab/ci/release-packages\.yml\s*$", ci, re.MULTILINE):
        problems.append(".gitlab-ci.yml does not include .gitlab/ci/release-packages.yml: nothing gathers what a release carries "
                        "from the package jobs")
    if chain.GATHERED != GATHER_JOB:
        problems.append("build/ci/release-chain.py collects the gathered files from %s/, not %s's artifact" % (chain.GATHERED, GATHER_JOB))
    if not gather:
        problems.append("%s has no %s job" % (FILES["release-packages"], GATHER_JOB))
    else:
        gathered_needs = need_entries(gather)
        wanted = []
        for build in chain.gathered_builds("9.9.9"):
            for role, artifacts, does in (("job", "true", "makes"), ("test", "false", "installs or runs")):
                name = build[role]
                if not name:
                    continue
                wanted.append(name)
                if name not in job_file:
                    problems.append("no package file has the job %s, which %s %s, a file the release carries" % (name, does, build["name"]))
                    continue
                entry = gathered_needs.get(name)
                if entry is None:
                    problems.append("%s does not need %s, which %s %s: the release would carry it past its build or its test"
                                    % (GATHER_JOB, name, does, build["name"]))
                    continue
                if re.search(r"optional: true", entry):
                    problems.append("%s needs %s optionally: a pipeline without it would release %s past it" % (GATHER_JOB, name, build["name"]))
                if not re.search(r"artifacts: %s" % artifacts, entry):
                    problems.append("%s must need %s with artifacts: %s" % (GATHER_JOB, name, artifacts))
            if build["test"] and build["job"] in everything and build["test"] in everything \
                    and build["job"] not in needs(everything[build["test"]]):
                problems.append("%s does not need %s, whose file it %s" % (build["test"], build["job"], "tests"))
        # Every job the gathered files wait for, and every job those wait for (the
        # AppImages' runtimes and the aarch64 compile among them): none may let the
        # release start past it on a tag, nor wait for a press on a batch* branch,
        # where the release is rehearsed.
        upstream, pending = set(), [n for n in wanted if n in everything]
        while pending:
            name = pending.pop()
            if name in upstream:
                continue
            upstream.add(name)
            pending.extend(n for n in needs(everything[name]) if n in everything)
        for name in sorted(upstream):
            for escape in tag_escapes(name, everything):
                problems.append("%s can let release start in a v* tag's pipeline without its having passed (%s): release "
                                "would carry what it makes past it" % (name, escape))
            deciding = [c for c in template_order(name, everything) if re.search(r"^  rules:", everything[c], re.MULTILINE)]
            for item in rule_items(everything[deciding[0]], everything) if deciding else []:
                condition = re.match(r"if:\s*(.+)", item.strip())
                if re.search(r"^\s*when:\s*manual\s*$", item, re.MULTILINE) and (
                        not condition or "$CI_COMMIT_BRANCH !~ /^batch/" not in condition.group(1) or "||" in condition.group(1)):
                    problems.append("%s waits for a press on a batch* branch (%s), where the release is rehearsed: the "
                                    "rehearsal would wait for it" % (name, condition.group(1) if condition else item.strip()))
        for name in sorted(set(gathered_needs) - set(wanted)):
            problems.append("%s needs %s, which makes nothing build/ci/release-chain.py gathers" % (GATHER_JOB, name))
        for escape in tag_escapes(GATHER_JOB, everything):
            problems.append("%s can let release start in a v* tag's pipeline without its having passed (%s)" % (GATHER_JOB, escape))
        if GATHER_REFUSAL not in gather:
            problems.append("%s is not made for a v* tag and a batch* branch alone, and there by .own-runners' rules, which "
                            "make the package jobs it needs: %s" % (GATHER_JOB, GATHER_REFUSAL.replace("\n", " ")))
        if "    - python3 build/ci/release-chain.py gather --out release-packages" not in gather or \
                not re.search(r"^      - release-packages/\s*$", gather, re.MULTILINE) or "  extends: .linix" not in gather:
            problems.append("%s does not run build/ci/release-chain.py gather on linix and keep release-packages/" % GATHER_JOB)
    release_entries = need_entries(jobs["release"])
    for name in (GATHER_JOB, CHART_JOB):
        entry = release_entries.get(name)
        if entry is not None and re.search(r"optional: true", entry):
            problems.append("release needs %s optionally: a tag's pipeline without it would be released without its files" % name)
    chart = blocks(texts["helm"]).get(CHART_JOB, "")
    if not chart or "- sh build/ci/helm-chart-package.sh" not in chart or not re.search(r"^      - helm-chart/\s*$", chart, re.MULTILINE) \
            or "$CI_COMMIT_TAG =~ /^v/" not in chart:
        problems.append("%s does not package the chart into helm-chart/ for a v* tag (build/ci/helm-chart-package.sh)" % CHART_JOB)
    for escape in tag_escapes(CHART_JOB, everything) if chart else []:
        problems.append("%s can let release start in a v* tag's pipeline without its having passed (%s)" % (CHART_JOB, escape))
    chart_name = chain.chart_name("9.9.9")
    if chain.sources("9.9.9").get(chart_name, "").replace(os.sep, "/") != "helm-chart/" + chart_name or \
            chain.package_job_sums("9.9.9").get(chart_name, ("", ""))[0].replace(os.sep, "/") != "helm-chart/SHA256SUMS.txt":
        problems.append("build/ci/release-chain.py does not collect %s from helm-chart/, held to helm-chart/SHA256SUMS.txt" % chart_name)

    # The AppImages go out with the source of the libraries linked into their runtime
    # (build/ci/packages/appimage-runtime-sources.sh; docs/PackagesAppImage.md,
    # "Licences"), which each AppImage names: gathered from its job, held to that job's
    # sums, signed like every primary file, with a line in the release's sums, and no
    # SBOM, being source.
    primaries = chain.primary_assets("9.9.9")
    sources_name = chain.runtime_sources_name("9.9.9")
    appimage_jobs = blocks(texts["packages-appimage"])
    sources_job = appimage_jobs.get(RUNTIME_SOURCES_JOB, "")
    if any(name.endswith(".AppImage") for name in primaries):
        found_builds = [b for b in chain.gathered_builds("9.9.9") if b["name"] == sources_name]
        if sources_name not in primaries or not found_builds:
            problems.append("the release carries the AppImages without %s, the source of the libraries linked into their "
                            "runtime, which the LGPL-2.1 of libfuse3 and the GPL of lzo ask to go with them "
                            "(build/ci/release-chain.py, gathered_builds)" % sources_name)
        else:
            build = found_builds[0]
            if (build["job"], build["path"], build["sums"], build["listed"]) != (
                    RUNTIME_SOURCES_JOB, RUNTIME_SOURCES_DIR + sources_name, RUNTIME_SOURCES_DIR + "SHA256SUMS.txt", sources_name):
                problems.append("build/ci/release-chain.py gathers %s from %s, made by %s, held to %s: not from %s's %s, held to "
                                "its SHA256SUMS.txt" % (sources_name, build["path"], build["job"], build["sums"],
                                                         RUNTIME_SOURCES_JOB, RUNTIME_SOURCES_DIR))
        if sources_name in chain.compiled_assets("9.9.9"):
            problems.append("build/ci/release-chain.py counts %s a compiled file: it is source, and goes out without an SBOM"
                            % sources_name)
        if "- sh build/ci/packages/appimage-runtime-sources.sh pack " not in sources_job \
                or not re.search(r"^      - %s\s*$" % re.escape(RUNTIME_SOURCES_DIR), sources_job, re.MULTILINE):
            problems.append("%s has no %s job that runs build/ci/packages/appimage-runtime-sources.sh pack and keeps %s"
                            % (FILES["packages-appimage"], RUNTIME_SOURCES_JOB, RUNTIME_SOURCES_DIR))
        for arch in chain.APPIMAGE_ARCHES:
            if sources_job and "appimage-runtime-" + arch not in needs(sources_job):
                problems.append("%s does not need appimage-runtime-%s, whose record of what it linked it packs the source of"
                                % (RUNTIME_SOURCES_JOB, arch))
            if RUNTIME_SOURCES_JOB not in needs(appimage_jobs.get("appimage-" + arch, "")):
                problems.append("appimage-%s does not need %s, whose licence files of the runtime's libraries the image "
                                "carries" % (arch, RUNTIME_SOURCES_JOB))

    # And with the source of the libraries they carry in usr/lib, copied from Ubuntu
    # (build/ci/packages/appimage-bundled-sources.sh; docs/PackagesAppImage.md,
    # "Licences"), which each image names in bundled.txt: gathered from its job, held to
    # that job's sums, signed like every primary file, with a line in the release's
    # sums, and no SBOM, being source; the job packing it from both AppImage jobs'
    # records; and the image, its test and the pack naming it as release-chain.py does.
    bundled_name = chain.bundled_sources_name("9.9.9")
    bundled_job = appimage_jobs.get(BUNDLED_SOURCES_JOB, "")
    if any(name.endswith(".AppImage") for name in primaries):
        found_builds = [b for b in chain.gathered_builds("9.9.9") if b["name"] == bundled_name]
        if bundled_name not in primaries or not found_builds:
            problems.append("the release carries the AppImages without %s, the source of the libraries they carry in usr/lib, "
                            "which the LGPL of GnuTLS, Nettle, GMP, libidn2, libunistring, libtasn1 and keyutils asks to be "
                            "offered with them (build/ci/release-chain.py, gathered_builds)" % bundled_name)
        else:
            build = found_builds[0]
            if (build["job"], build["path"], build["sums"], build["listed"]) != (
                    BUNDLED_SOURCES_JOB, BUNDLED_SOURCES_DIR + bundled_name, BUNDLED_SOURCES_DIR + "SHA256SUMS.txt", bundled_name):
                problems.append("build/ci/release-chain.py gathers %s from %s, made by %s, held to %s: not from %s's %s, held to "
                                "its SHA256SUMS.txt" % (bundled_name, build["path"], build["job"], build["sums"],
                                                         BUNDLED_SOURCES_JOB, BUNDLED_SOURCES_DIR))
        if bundled_name in chain.compiled_assets("9.9.9"):
            problems.append("build/ci/release-chain.py counts %s a compiled file: it is source, and goes out without an SBOM"
                            % bundled_name)
        if "- bash build/ci/packages/appimage-bundled-sources.sh pack " not in bundled_job \
                or not re.search(r"^      - %s\s*$" % re.escape(BUNDLED_SOURCES_DIR), bundled_job, re.MULTILINE):
            problems.append("%s has no %s job that runs build/ci/packages/appimage-bundled-sources.sh pack and keeps %s"
                            % (FILES["packages-appimage"], BUNDLED_SOURCES_JOB, BUNDLED_SOURCES_DIR))
        pack_lines = re.findall(r"^    - bash build/ci/packages/appimage-bundled-sources\.sh pack (.*)$", bundled_job, re.MULTILINE)
        for arch in chain.APPIMAGE_ARCHES:
            if bundled_job and ("appimage-" + arch not in needs(bundled_job)
                                or not pack_lines or "appimage/built-against-%s" % arch not in pack_lines[0].split()):
                problems.append("%s does not pack from appimage-%s's record of what its image carries (it must need the job "
                                "and pack appimage/built-against-%s)" % (BUNDLED_SOURCES_JOB, arch, arch))
        # The name each place writes, for version 9.9.9 as each spells the version: the
        # list the image carries, the test's hold on it, and the file the pack makes.
        named = (("appimage-pack", 'BUNDLED_SOURCES="%s"', "${VERSION}", "the image's bundled.txt"),
                 ("appimage-test", 'grep -Fqx "sources %s"', "$expected", "the image's test"),
                 ("bundled-sources", 'name="%s"', "$version", "the pack"))
        for key, form, spelling, what in named:
            if form % chain.bundled_sources_name(spelling) not in texts[key]:
                problems.append("%s (%s) does not name %s as build/ci/release-chain.py does: the image would name a file "
                                "the release does not carry" % (FILES[key], what, chain.bundled_sources_name(spelling)))
        if "tr -d '\\r' < \"${REPO_ROOT}/LICENSE\" > \"${APPDIR}/usr/share/hmailserver/licenses/AGPL-3.0.txt\"" \
                not in texts["appimage-pack"] \
                or 'agpl="$root/usr/share/hmailserver/licenses/AGPL-3.0.txt"' not in texts["appimage-test"] \
                or 'grep -qx " *GNU AFFERO GENERAL PUBLIC LICENSE" "$agpl"' not in texts["appimage-test"]:
            problems.append("%s or %s no longer puts hMailServer's own AGPL-3.0 text in the image, or holds it there"
                            % (FILES["appimage-pack"], FILES["appimage-test"]))
    # Both sources jobs fetch from outside the project - Alpine's GitLab and distfiles
    # mirror, Ubuntu's archive and snapshot service - by recipes, five of the Alpine
    # APKBUILDs naming GitHub as their source, and neither script is to follow one
    # there: .no-github keeps GitHub out of the job itself, on any runner and in any
    # image, which the runner's hosts entries alone do not.
    no_github_jobs = dict(blocks(texts["ci"]))
    no_github_jobs.update(appimage_jobs)
    no_github_jobs.update(blocks(texts["no-github"]))
    for job in (RUNTIME_SOURCES_JOB, BUNDLED_SOURCES_JOB):
        if job in appimage_jobs and not takes_no_github(job, no_github_jobs):
            problems.append("%s in %s does not keep GitHub out of itself: .no-github (%s) is not in its extends, or a "
                            "before_script of its own or of a nearer template replaces .no-github's without beginning with "
                            "!reference [.no-github, before_script]" % (job, FILES["packages-appimage"], FILES["no-github"]))
    # Every Linux file the release carries, the chart and the AppImages' two sources
    # files, with a line in the release's sums: linux-build's own two, then sums_members.
    # Not the installers and the PowerShell module, which Authenticode signs after collect
    # writes the sums.
    names_999 = feed.installer_names("9.9.9")
    unsummed = sorted(set(primaries) - set(chain.sums_members("9.9.9")) - {
        names_999["windows"], names_999["windows-arm64"], names_999["deb-amd64"], names_999["rpm-x86_64"], chain.sums_name("9.9.9"),
        "hmailserver.spdx.json", "hmailserver.cyclonedx.json"} - set(getattr(chain, "MODULE_FILES", ())))
    if unsummed:
        problems.append("the release carries %s with no line in the release's sums (build/ci/release-chain.py, sums_members)"
                        % ", ".join(unsummed))

    # No job needs more jobs than GitLab takes: the pipeline would not be created.
    for key in JOB_FILES:
        for name, text in sorted(blocks(texts[key]).items()):
            count = len(need_entries(text)) or len(needs(text))
            if count > NEEDS_LIMIT:
                problems.append("%s in %s needs %d jobs; GitLab.com takes %d (ci_needs_size_limit), and would create no "
                                "pipeline at all" % (name, FILES[key], count, NEEDS_LIMIT))

    # The rehearsal: a batch* branch's alone, the four commands, one token (Sigstore's),
    # and nothing that reads or writes the registry or the releases.
    rehearsal = blocks(texts["rehearsal"])
    if not re.search(r"^  - local: \.gitlab/ci/release-rehearsal\.yml\s*$", ci, re.MULTILINE):
        problems.append(".gitlab-ci.yml does not include .gitlab/ci/release-rehearsal.yml: the release is not rehearsed")
    if REHEARSAL_REFUSAL not in rehearsal.get(REHEARSAL_RULES, "") or \
            len(re.findall(r"^    - ", rehearsal.get(REHEARSAL_RULES, ""), re.MULTILINE)) != 2:
        problems.append("%s is not exactly: never for a merge request, a tag, master or any branch but batch*, then "
                        ".own-runners' rules: %s" % (REHEARSAL_RULES, REHEARSAL_REFUSAL.replace("\n", " ")))
    for name, text in sorted(rehearsal.items()):
        if name.startswith("."):
            continue
        if name not in REHEARSAL_JOBS:
            problems.append("%s has a job %s, which is not the rehearsal's" % (FILES["rehearsal"], name))
            continue
        wanted_needs, commands = REHEARSAL_JOBS[name]
        if not re.search(r"^  rules:\s*\n    - !reference \[%s, rules\]\s*$" % re.escape(REHEARSAL_RULES), text, re.MULTILINE) or \
                len(re.findall(r"^    - ", text.split("  rules:\n", 1)[-1].split("\n  ", 1)[0] + "\n", re.MULTILINE)) != 1:
            problems.append("%s's rules are not %s alone" % (name, REHEARSAL_RULES))
        have = needs(text)
        for need in wanted_needs:
            if need not in have:
                problems.append("%s does not need %s (it needs %s)" % (name, need, ", ".join(have) or "nothing"))
        script = re.search(r"^  script:\s*\n((?:    - .*\n?)+)", text + "\n", re.MULTILINE)
        ran = [line.strip()[2:] for line in (script.group(1) if script else "").split("\n") if line.strip().startswith("- ")]
        if [c for c in ran if not c.startswith("mkdir -p release-tools && cp ")] != commands:
            problems.append("%s runs %s, not %s" % (name, ran, commands))
        for word in NOT_IN_A_REHEARSAL:
            if word in text:
                problems.append("%s names %s: a rehearsal reads and publishes nothing of the registry or the releases" % (name, word))
        tokens = re.search(r"^  id_tokens:\s*\n((?:    .*\n?)+)", text + "\n", re.MULTILINE)
        if name == "release-rehearsal-sign":
            if not tokens or not re.search(r"^    SIGSTORE_ID_TOKEN:\s*\n\s+aud: sigstore\s*$", tokens.group(1), re.MULTILINE) \
                    or len(re.findall(r"^    [A-Z_]+:", tokens.group(1), re.MULTILINE)) != 1:
                problems.append("release-rehearsal-sign holds no id_tokens SIGSTORE_ID_TOKEN with aud sigstore, or another token besides")
            if not re.search(r"^    name: \$HM_COSIGN_IMAGE\s*$", text, re.MULTILINE):
                problems.append("release-rehearsal-sign's image is not $HM_COSIGN_IMAGE")
        elif tokens:
            problems.append("%s holds an ID token: only the rehearsal's signing job signs, and only with Sigstore's" % name)
    for name in REHEARSAL_JOBS:
        if name not in rehearsal:
            problems.append("%s has no %s job" % (FILES["rehearsal"], name))
    main_text = function_text(texts["chain"], "main")
    if 'if args.rehearse and args.command not in ("collect", "sbom", "check-provenance"):' not in main_text or \
            'problems = collect(ROOT, None, os.path.join(ROOT, args.out or "release-assets"), os.environ, api=None, rehearse=True)' not in main_text:
        problems.append("build/ci/release-chain.py takes --rehearse with a command that publishes or gathers, or gives a "
                        "rehearsal's collect the job's token")

    # The feed follows the release in the tag's own pipeline: pages waits for
    # release-publish there, and package-repos, which needs pages, deploys the site.
    pages_job = jobs.get("pages", "")
    pages_needs = need_entries(pages_job)
    publish_need = pages_needs.get("release-publish", "")
    if TAG_ONLY not in rules_text("pages", jobs):
        problems.append("pages does not run for a protected v* tag (%s): the tag's pipeline would not deploy the feed with "
                        "its release in it" % TAG_ONLY)
    if not publish_need or not re.search(r"optional: true", publish_need) or not re.search(r"artifacts: false", publish_need) \
            or sorted(pages_needs) != ["release-publish"]:
        problems.append("pages does not need release-publish, optionally and without its artifacts, and nothing else (it "
                        "needs %s): in a v* tag's pipeline the feed must be rendered once the release exists, and on master, "
                        "where release-publish is not, at once" % (sorted(pages_needs) or "nothing"))
    repos_job = blocks(texts["repos"]).get("package-repos", "")
    if not re.search(r"artifacts: true", need_entries(repos_job).get("pages", "")) or not re.search(r"^  pages: true\s*$", repos_job, re.MULTILINE):
        problems.append("package-repos does not need pages with its artifacts, or does not deploy the site (pages: true): the "
                        "release's feed would not reach Pages after release-publish")
    # No trigger of a project: GitLab drops one naming its own project
    # (invalid_bridge_trigger, Ci::CreateDownstreamPipelineService), as it dropped
    # 6.3.5's update-feed, and a path breaks when the project moves into the group.
    for where, text in [(".gitlab-ci.yml", ci)] + sorted(texts.get("ci-includes", {}).items()):
        for name, block in sorted(blocks(text).items()):
            if re.search(r"^  trigger:[ \t]*(?:[^\s#{]|\{[^}\n]*project:)", block, re.MULTILINE) or re.search(r"^  trigger:\s*\n(?:    .*\n)*?    project:", block + "\n", re.MULTILINE):
                problems.append("%s in %s triggers a pipeline of a project (trigger: project): GitLab refuses a multi-project "
                                "trigger into the project that runs it (invalid_bridge_trigger, as 6.3.5's update-feed was), "
                                "and a project's path changes when it moves into the group" % (name, where))
    # Every job (or template) that names bench150 stops, before its checkout, what a
    # cancelled job left running in its directory: windows-installer and
    # release-authenticode are a release's (.gitlab-ci.yml, .bench150-leftovers).
    for where, text in [(".gitlab-ci.yml", ci)] + sorted(texts.get("ci-includes", {}).items()):
        for name, block in sorted(blocks(text).items()):
            if re.search(r"^  tags:[ \t]*\[[^\]\n]*\bbench150\b", block, re.MULTILINE) and \
                    not re.search(r"^  hooks: !reference \[\.bench150-leftovers, hooks\]\s*$", block, re.MULTILINE):
                problems.append("%s in %s runs on bench150 without hooks: !reference [.bench150-leftovers, hooks]: a compiler a "
                                "cancelled job left running there can hold a file its checkout must replace" % (name, where))
    if "pre_get_sources_script:" not in jobs.get(".bench150-leftovers", ""):
        problems.append(".gitlab-ci.yml's .bench150-leftovers has no hooks: pre_get_sources_script, which the bench150 jobs take")
    for name, text in sorted(jobs.items()):
        if name.startswith(".") or name in RESERVED or name in RELEASE_JOBS:
            continue
        rules = rules_text(name, jobs)
        if name in EXEMPT:
            needle, why = EXEMPT[name]
            if needle not in rules:
                problems.append("%s is exempt from the refusal because it runs for %s, but its rules no longer say %s" % (name, why, needle))
            continue
        if not re.search(r"- if: " + re.escape(REFUSAL) + r"[^\n]*\n\s+when: never", rules):
            problems.append("%s would run in a downstream pipeline (source pipeline, on master) or a schedule's: "
                            "its rules must begin with - if: %s, when: never" % (name, REFUSAL))

    # The schedules: each chosen by its description, one way in.
    security = texts["security"]
    security_jobs = blocks(security)
    for description, (job, where) in sorted(ONE_JOB_SCHEDULES.items()):
        chosen = '$CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "%s"' % description
        owner = jobs if where == "ci" else blocks(texts[where])
        if chosen not in owner.get(job, ""):
            problems.append("%s does not run for the schedule described \"%s\" (%s): a schedule carries no variable on this "
                            "project, so its description is what chooses the job" % (job, description, chosen))
        refused = re.search(r'- if: \$CI_PIPELINE_SOURCE == "schedule" && \(([^\n]*)\)\n\s+when: never', jobs["pages"])
        if not refused or '$CI_PIPELINE_SCHEDULE_DESCRIPTION == "%s"' % description not in refused.group(1):
            problems.append("pages would run in the pipeline of the schedule described \"%s\" too, which is %s's alone: "
                            "it refuses a schedule of that description, when: never" % (description, job))
    # The pins asked of their registries, daily, in Renovate's schedule's pipeline: a
    # digest a registry no longer serves fails that pipeline, and not first the job that
    # pulls it, a release's among them (build/check-image-pins.py).
    pins_job = jobs.get("image-pins", "")
    if not pins_job:
        problems.append("there is no image-pins job (.gitlab-ci.yml): a digest a registry no longer serves would be found "
                        "only by the job that pulls it, a release tag's among them")
    else:
        if '$CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "renovate" && $CI_COMMIT_REF_PROTECTED == "true"' \
                not in rules_text("image-pins", jobs):
            problems.append("image-pins does not run in the pipeline of the schedule described \"renovate\" on a protected ref, "
                            "daily, beside renovate")
        if not re.search(r"^    - python3 build/check-image-pins\.py --self-test\s*$", pins_job, re.MULTILINE) or \
                not re.search(r"^    - python3 build/check-image-pins\.py\s*$", pins_job, re.MULTILINE):
            problems.append("image-pins does not run build/check-image-pins.py's self-test and then the check itself over the "
                            "tree, each as a line of its script")
        if re.search(r"^  allow_failure:", pins_job, re.MULTILINE):
            problems.append("image-pins says allow_failure: a pin gone from its registry must fail the pipeline")
        if not re.search(r"^  retry:\n    max: 2\n    when: !reference \[\.gate-retry, retry, when\]\n    exit_codes: \[2\]\n",
                         pins_job + "\n", re.MULTILINE):
            problems.append("image-pins is not retried twice on exit 2, a registry that could not be asked, and on "
                            ".gate-retry's reasons alone: a retry on every failure would ask again about a pin that is gone")
    repos_job = blocks(texts["repos"]).get("package-repos", "")
    if REPOS_RULES not in repos_job or len(re.findall(r"^    - ", repos_job.split("  rules:\n", 1)[-1].split("\n  script:")[0], re.MULTILINE)) != 1:
        problems.append("package-repos's rules are not pages' alone (%s): it would deploy the site in a pipeline "
                        "pages refuses, the schedules of one job each among them" % REPOS_RULES.strip())
    # The package files too, whose build rules refuse before they make a job by hand: a
    # refusal that reads an old route refuses a build in a pipeline where .own-runners still
    # makes the install job that needs it, and GitLab then creates no pipeline at all.
    packages = [(key, blocks(texts[key])) for key in ("packages-deb", "packages-rpm", "packages-appimage", "packages-windows")]
    for key, found in [("ci", jobs), ("security", security_jobs), ("export", blocks(texts["export"]))] + packages:
        for name, text in sorted(found.items()):
            for old in OLD_ROUTES:
                if "$" + old in text:
                    problems.append("%s in %s still reads $%s: a schedule is chosen by its description alone (the project "
                                    "refuses a schedule's variables), and one way in is all there is" % (name, FILES[key], old))

    # A push that changes documentation alone starts nothing on the project's own
    # runners, and one that changes anything else starts everything there.
    anchor = re.search(r"^        paths: &%s\s*\n((?:          - \"[^\"\n]+\"\s*\n)+)" % NOT_DOCS_ONLY, jobs.get(".own-runners", "") + "\n",
                       re.MULTILINE)
    if not anchor:
        problems.append(".own-runners has no rules:changes paths: &%s under its protected-ref rule: a push that changes only "
                        "documentation would start the gates" % NOT_DOCS_ONLY)
    else:
        patterns = re.findall(r'- "([^"\n]+)"', anchor.group(1))
        try:
            compiled = [glob_regex(p) for p in patterns]
        except ValueError as error:
            compiled = []
            problems.append(".own-runners' paths: %s" % error)
        if compiled:
            skipped_code = [f for f in texts["tree"] if not docs_only(f) and not any(c.match(f) for c in compiled)]
            started_by_docs = [f for f in texts["tree"] if docs_only(f) and any(c.match(f) for c in compiled)]
            if skipped_code:
                problems.append(".own-runners' paths match none of %d file(s) that are not documentation, so a push changing only "
                                "those would skip the gates: %s" % (len(skipped_code), ", ".join(skipped_code[:8])))
            if started_by_docs:
                problems.append(".own-runners' paths match %d Markdown file(s) at the root or under .gitlab/, so a push changing "
                                "only documentation would start the gates: %s" % (len(started_by_docs), ", ".join(started_by_docs[:8])))
            if not any(f.startswith("hmailserver/docs/") and f.endswith(".md") for f in texts["tree"]):
                problems.append("the tree has no hmailserver/docs/*.md to check the paths against")
    for name in sorted(jobs):
        if name.startswith(".") or name in RESERVED or name in RELEASE_JOBS or name in SCHEDULE_ONLY_JOBS:
            continue
        if not re.search(r"^  tags: \[(linix|bench150|%s)\]" % "|".join(re.escape("$" + v) for v in sorted(RUNNER_CLASSES)),
                         inherited(name, jobs), re.MULTILINE):
            continue
        rules = rules_text(name, jobs)
        if "paths: &%s" % NOT_DOCS_ONLY not in rules and "paths: *%s" % NOT_DOCS_ONLY not in rules:
            problems.append("%s runs on the project's own runners for a push that changes only documentation: its rules "
                            "must be .own-runners', or name paths: *%s" % (name, NOT_DOCS_ONLY))

    # The long gates are not cut off by a later push, and a busy bench is a warning
    # only where no release can follow.
    for name in NOT_INTERRUPTIBLE:
        if not re.search(r"^  interruptible: false\s*$", jobs.get(name, ""), re.MULTILINE):
            problems.append("%s can be cancelled half-way by a later push: it says interruptible: false" % name)
    for name in gates(jobs):
        if re.search(r"^  allow_failure:\s*\n\s+exit_codes:", inherited(name, jobs), re.MULTILINE) and BUSY_GUARD not in jobs[name]:
            problems.append("%s lets an exit code fail without failing the pipeline, and release waits for it: that code must "
                            "never come from a v* tag's pipeline (%s)" % (name, BUSY_GUARD))

    # The fuzz job keeps what it finds, for the project's members only.
    fuzz = jobs.get("linux-fuzz", "")
    kept = re.search(r"^  artifacts:\s*\n((?:    .*\n?)+)", fuzz, re.MULTILINE)
    kept = kept.group(1) if kept else ""
    if "- fuzz-results/" not in kept or "when: always" not in kept or "access: developer" not in kept:
        problems.append("linux-fuzz does not keep fuzz-results/ as its artifact whatever the result (when: always) for "
                        "Developers only (access: developer): a crash's reproducer would be lost, or public")
    for name, text in sorted(jobs.items()):
        if name != "linux-fuzz" and "fuzz-results/" in text:
            problems.append("%s claims fuzz-results/, which linux-fuzz alone writes" % name)
    # Nor does its log, which anyone can read, carry a finding: libFuzzer ends its report
    # with the input (up to 256 bytes in hex and Base64), and a sanitizer's SUMMARY names
    # the place. The script prints the kind alone and names the report.
    finding = re.search(r"FINDING \(exit \$code\)(.*?)\n    return 1\n", texts["fuzz"], re.DOTALL)
    if not finding or re.search(r"\b(tail|cat|xargs|less|xxd|od)\b", finding.group(1)):
        problems.append("build/ci/linux-fuzz.sh prints a finding's report into the job log, which is public: the input and "
                        "the place belong in fuzz-results/, the Developers-only artifact")
    if "s/^(SUMMARY: (AddressSanitizer|UndefinedBehaviorSanitizer|MemorySanitizer): [A-Za-z-]+) .*/\\1/" not in texts["fuzz"]:
        problems.append("build/ci/linux-fuzz.sh prints a sanitizer's SUMMARY whole into the public job log: the place of a "
                        "finding not yet fixed stays in the artifact")

    # The security scans hold the release: release waits for each, brings its report,
    # and collect asks the gate before anything is copied, let alone signed.
    if sorted(SCANNED) != sorted(chain.security_gate.SCANS):
        problems.append("build/ci/security-gate.py reads the scans %s; release needs %s" % (
            ", ".join(chain.security_gate.SCANS), ", ".join(SCANNED)))
    release_needs = need_entries(jobs["release"])
    for name in SCANNED:
        entry = release_needs.get(name)
        if entry is None:
            problems.append("release does not need %s: the release would be collected without reading that scan's report "
                            "(build/ci/security-gate.py)" % name)
            continue
        if not re.search(r"artifacts: true", entry):
            problems.append("release does not bring %s's artifacts: the gate would find no report and hold every release" % name)
        if re.search(r"optional: true", entry):
            problems.append("release needs %s optionally: a tag's pipeline without that scan would be released past it" % name)
    for name in BLOCKING_SCANS:
        text = inherited(name, jobs) + "\n" + rules_text(name, jobs)
        # Any allow_failure but false: true, exit codes in either style, and YAML 1.1's
        # other spellings of true (yes, on), which GitLab's parser reads as true too.
        if name not in jobs or re.search(r"^\s*allow_failure:(?![ \t]*false[ \t]*$)", text, re.MULTILINE):
            problems.append("%s may fail without failing the pipeline (allow_failure): it holds its findings to a "
                            "baseline so that a new one fails a branch's pipeline, and it must (docs/ContinuousIntegration.md, "
                            "'Security scanning')" % name)
    sast = blocks(texts["security"]).get("semgrep-sast", "")
    if "- if: " + TAG_ONLY not in sast:
        problems.append("semgrep-sast does not run for a protected v* tag (- if: %s): the release job needs its report, "
                        "and no pipeline of master's need have scanned the tagged commit" % TAG_ONLY)
    kept = re.search(r"^  artifacts:\s*\n((?:    .*\n?)+)", sast, re.MULTILINE)
    kept = kept.group(1) if kept else ""
    if "- gl-sast-report.json" not in kept or "when: always" not in kept:
        problems.append("semgrep-sast does not keep gl-sast-report.json as a file (artifacts: paths, when: always): a "
                        "later job's needs bring paths, not reports, and the release job reads it")
    collect_text = function_text(texts["chain"], "collect")
    asked = collect_text.find("problems.extend(security_gate.evaluate(root, root")
    if "exceptions, missing = master_exceptions(root, run)" not in collect_text or "exceptions_text=exceptions" not in collect_text:
        problems.append("build/ci/release-chain.py's collect does not give the gate master's .gitlab/security-exceptions.yml: "
                        "the tagged tree's own copy could accept what master does not")
    copied = collect_text.find("shutil.copyfile(")
    if asked < 0 or copied < asked or collect_text.find("    if problems:\n        return problems", asked) < 0:
        problems.append("build/ci/release-chain.py's collect no longer asks build/ci/security-gate.py before it copies the "
                        "release's files, or goes on past what the gate says")
    if "problems.extend(security_gate.committed_reports(root, run))" not in collect_text or \
            "not_before=security_gate.pipeline_created(env)" not in collect_text:
        problems.append("build/ci/release-chain.py's collect does not refuse a tagged tree that commits a scan's report, or "
                        "a semgrep-sast report older than the tag's pipeline: either would stand in for a report the scan "
                        "did not write")
    entries, wrong = chain.security_gate.check_exceptions(texts["exceptions"])
    problems.extend(wrong)
    owners = dict((line.split()[0], line.split()[1:]) for line in texts["codeowners"].split("\n")
                  if line.strip() and not line.lstrip().startswith("#") and len(line.split()) > 1)
    for path in OWNED:
        if owners.get(path) != ["@Progressiverobot"]:
            problems.append(".gitlab/CODEOWNERS does not give %s to @Progressiverobot alone: what may pass the release's "
                            "security gate, or the secret scan, is the owner's to accept" % path)

    # Secret detection fails on a finding.
    detection = security_jobs.get("secret_detection", "")
    if not re.search(r"^  allow_failure: false\s*$", detection, re.MULTILINE):
        problems.append("secret_detection does not say allow_failure: false: GitLab's template makes it true, and a secret "
                        "found would be a warning")
    if "- /analyzer run" not in detection or "\"vulnerabilities\" *: *\\[ *\\]" not in detection or "exit 1" not in detection:
        problems.append("secret_detection does not read its report after the analyzer, which exits 0 whatever it finds: "
                        "the job must fail unless the report's vulnerabilities are []")
    if not re.search(r'== "security"[^\n]*\n\s+variables:\s*\n\s+SECRET_DETECTION_HISTORIC_SCAN: "true"', detection):
        problems.append("secret_detection does not scan the whole history for the schedule described \"security\"")
    mirrored = MIRRORED_SECRETS.search(detection)
    if not mirrored:
        problems.append("secret_detection does not run GitLab's analyzer as the project's registry holds it, on one line "
                        "(image: registry.gitlab.com/hmailserver/hmailserver/mirror/secrets:<version>-<12 hex>@sha256:<digest>, "
                        "build/ci/mirror-secrets.sh): GitLab's registry stops serving a digest its tag moves on from, which "
                        "would stop the job and the landing's secret scan (build/scan-secrets.py), which takes this image")
    elif not mirrored.group(2).startswith(mirrored.group(1)):
        problems.append("secret_detection's image tag ends in %s, and it is named by the digest %s...: the copy's tag is "
                        "the digest's first twelve hex digits (build/ci/mirror-secrets.sh)"
                        % (mirrored.group(1), mirrored.group(2)[:12]))
    if not os.path.isfile(os.path.join(ROOT, ".gitleaksignore")):
        problems.append(".gitleaksignore is missing: the findings read and found not to be secrets would fail secret_detection")
    if SECRET_RULES_STEP not in detection:
        problems.append("secret_detection does not put the rules build/ci/secret-detection-rules.sh derives from GitLab's "
                        "in place of /gitleaks.toml right before /analyzer run (%s): GitLab's own allowlist leaves out "
                        "every path under a directory named build, bin, obj, target or packages, this tree's build/ among "
                        "them"
                        % " then ".join(l.strip()[2:] for l in SECRET_RULES_STEP.strip().split("\n")))
    if not os.path.isfile(os.path.join(ROOT, "build", "ci", "secret-detection-rules.sh")):
        problems.append("build/ci/secret-detection-rules.sh is missing: secret_detection and build/scan-secrets.py make "
                        "their rules with it")

    found, said = change_gate_problems(texts, jobs, security_jobs, chain.security_gate)
    problems.extend(found)
    notes.extend(said)
    found, said = runner_problems(texts)
    problems.extend(found)
    notes.extend(said)
    problems.extend(runner_class_problems(texts))
    return problems, notes


def sast_scope_problems(texts, security_jobs, gates_jobs, security_gate):
    """What would let semgrep-sast's report be another's, or its scope narrow unseen:
    semgrep-sast allow_failure, not removing what a checkout leaves of its outputs, not
    keeping Semgrep's SARIF, an exclusion not held to the root, a narrowing variable a
    CI file defines, a CI file that defines a variable GitLab gives, a file of the tree
    that narrows the scan or stands in for its report, and a workflow whose first rule
    names other branches than the gates run for."""
    problems = []
    sast = security_jobs.get(GATE_SCAN, "")
    if not re.search(r"^  allow_failure: false\s*$", sast, re.MULTILINE):
        problems.append("semgrep-sast does not say allow_failure: false: GitLab's template makes it true, and a scan that "
                        "failed would let a report the job did not write reach semgrep-sast-gate")
    if SAST_SCRIPT not in sast + "\n":
        problems.append("semgrep-sast does not remove gl-sast-report.json and semgrep.sarif before /analyzer run "
                        "(%r): a copy the checkout carries would be kept as the scan's" % SAST_SCRIPT)
    kept = re.search(r"^  artifacts:\s*\n((?:    .*\n?)+)", sast, re.MULTILINE)
    if not kept or not re.search(r"^      - semgrep\.sarif\s*$", kept.group(1), re.MULTILINE):
        problems.append("semgrep-sast does not keep semgrep.sarif (artifacts: paths): semgrep-sast-gate reads from it "
                        "which files the scan gave up on")
    if security_gate is None:
        return problems
    # Every CI file, each of FILES as texts holds it (so that the self-test's breakings
    # are read), the other included files as the tree has them.
    every_ci = dict(texts.get("ci-includes", {}))
    for key, relative in FILES.items():
        if relative.replace(os.sep, "/") in every_ci and key in texts:
            every_ci[relative.replace(os.sep, "/")] = texts[key]
    every_ci[".gitlab-ci.yml"] = texts["ci"]
    security_file = FILES["security"].replace(os.sep, "/")
    entries, readable = security_gate.exclusions(security_gate.scope_variables({security_file: texts["security"]}))
    loose = [e for e in entries if not security_gate._pattern_parts(e)[0]]
    if not readable or not entries or loose:
        problems.append("semgrep-sast's SAST_EXCLUDED_PATHS has %s: an entry with no slash leaves out a directory of that "
                        "name at any depth (Semgrep's --exclude and GitLab's filter alike), so begin each with a slash or "
                        "name its path" % (", ".join(repr(e) for e in loose) if loose else "no entry this can read"))
    for path, text in sorted(every_ci.items()):
        defined = security_gate.scope_variables({path: text})
        for name in SAST_NARROWING_VARIABLES:
            values = defined.get(name, [])
            if not values:
                continue
            if path == security_file and name == "SAST_SEMGREP_EXCLUDED_PATHS" and values == [""]:
                continue
            problems.append("%s defines %s (%s): it would narrow what semgrep-sast or static-analysis reads unseen; only "
                            "semgrep-sast's job sets SAST_SEMGREP_EXCLUDED_PATHS, to nothing" % (
                                path, name, ", ".join(repr(v) for v in values)))
        if path != security_file and defined.get("SAST_EXCLUDED_PATHS"):
            problems.append("%s defines SAST_EXCLUDED_PATHS: semgrep-sast's own job says what it leaves out, in %s alone"
                            % (path, security_file))
    if not any(v == [""] for p, t in every_ci.items() if p == security_file
               for v in [security_gate.scope_variables({p: t}).get("SAST_SEMGREP_EXCLUDED_PATHS")]):
        problems.append("semgrep-sast does not set SAST_SEMGREP_EXCLUDED_PATHS to nothing itself (SAST_SEMGREP_EXCLUDED_PATHS: "
                        "\"\"): a value a CI file gives every job would reach the analyzer as Semgrep's --exclude")
    for path, name in security_gate.ci_variable_definitions(every_ci):
        problems.append("%s defines %s, a variable GitLab gives a pipeline: a CI file's definition stands in for GitLab's, "
                        "and the gates read these to tell which pipeline this is and what a change's base is" % (path, name))
    carried = [f for f in texts.get("tree", []) if NOT_IN_THE_TREE.search(f)]
    if carried:
        problems.append("the tree carries %s: a .semgrepignore or GitLab's custom ruleset narrows semgrep-sast, GitLab's "
                        "secret-detection ruleset would stand in for the rules secret_detection makes (where GitLab "
                        "reads one), and a committed report stands in for one a scan did not write" % ", ".join(carried[:5]))
    workflow = blocks(texts["ci"]).get("workflow", "")
    first = re.search(r"^  rules:[ \t]*\n    (- if: .*\n      when: never)", workflow + "\n", re.MULTILINE)
    if not first or first.group(1) != WORKFLOW_BATCH_RULE:
        problems.append(".gitlab-ci.yml's first workflow rule is not %r: a merge request from a fork's protected branch, "
                        "or from a protected branch here that runs no gate, would run no pipeline of its own"
                        % WORKFLOW_BATCH_RULE)
    workflow_branches = re.findall(r"CI_MERGE_REQUEST_SOURCE_BRANCH_NAME =~ (/[^/\n]+/)", first.group(1) if first else "")
    gate_branches = set(re.findall(r"CI_COMMIT_BRANCH =~ (/[^/\n]+/)", "\n".join(
        rules_text(name, gates_jobs) for name in CHANGE_GATES) + "\n" + rules_text(GATE_SCAN, security_jobs)))
    if len(workflow_branches) != 1 or gate_branches != set(workflow_branches):
        problems.append("the branches whose merge requests run no pipeline of their own (%s) are not the ones the gates run "
                        "for in a branch's pipeline (%s): a merge request from one of the others would land with no gate"
                        % (", ".join(workflow_branches) or "none named", ", ".join(sorted(gate_branches)) or "none"))
    return problems


def change_gate_problems(texts, jobs, security_jobs, security_gate=None):
    """(problems, notes): the two change gates run on every change, blocking, with no
    switch and no trigger list, against a base they find themselves, retried on what is
    not their answer; semgrep-sast runs where the first needs it, from the project's
    copy of the analyzer, blocking, with its report its own and nothing narrowing it
    unseen; no CI file defines a variable GitLab gives; the workflow's first rule names
    the branches the gates run for; the scans on linix judge a batch against master;
    and the jobs build/land-via-mr.py requires are jobs of the CI files."""
    problems, notes = [], []
    gates_jobs = blocks(texts["mr-checks"])
    every = dict(security_jobs, **gates_jobs)
    problems.extend(sast_scope_problems(texts, security_jobs, gates_jobs, security_gate))

    # semgrep-sast: on a batch* branch too, never switched off, from the mirror, retried.
    def has_rule(text, rule):
        return re.search(r"^\s*%s\s*$" % re.escape(rule), text, re.MULTILINE) is not None

    sast = security_jobs.get(GATE_SCAN, "")
    sast_rules = rules_text(GATE_SCAN, security_jobs)
    if not has_rule(sast, GATE_SCAN_BATCH_RULE):
        problems.append("semgrep-sast does not run on a protected batch* branch (%s): semgrep-sast-gate needs its report "
                        "there, and a needs of a job the pipeline does not have keeps the pipeline from being made"
                        % GATE_SCAN_BATCH_RULE)
    if "SAST_DISABLED" in sast_rules:
        problems.append("semgrep-sast's rules read SAST_DISABLED: a project variable would take a blocking gate's scan away "
                        "unseen")
    mirrored = MIRRORED_SAST.search(sast)
    if not mirrored:
        problems.append("semgrep-sast does not run GitLab's analyzer as the project's registry holds it "
                        "(registry.gitlab.com/hmailserver/hmailserver/mirror/semgrep:<version>-<12 hex>@sha256:<digest>, "
                        "build/ci/mirror-semgrep.sh): GitLab's registry stops serving a digest its tag moves on from, "
                        "and the gate would stop every change")
    elif not mirrored.group(2).startswith(mirrored.group(1)):
        problems.append("semgrep-sast's image tag ends in %s, and it is named by the digest %s...: the copy's tag is the "
                        "digest's first twelve hex digits (build/ci/mirror-semgrep.sh)" % (mirrored.group(1), mirrored.group(2)[:12]))
    retry = security_jobs.get(".gate-retry", "")
    missing = [reason for reason in GATE_RETRY_REASONS if not re.search(r"^      - %s\s*$" % reason, retry, re.MULTILINE)]
    if not re.search(r"^    max: 2\s*$", retry, re.MULTILINE) or missing:
        problems.append(".gate-retry (.gitlab/ci/gitlab-security.yml) does not retry twice on %s: a runner's or the "
                        "registry's failure would fail a blocking gate" % (", ".join(missing) or "max: 2"))
    if "stuck_or_timeout_failure" in retry:
        problems.append(".gate-retry names stuck_or_timeout_failure, which GitLab has split, and warns of, since 19.1: name "
                        "the reasons it was split into")
    if not re.search(r"^  retry: !reference \[\.gate-retry, retry\]\s*$", sast, re.MULTILINE):
        problems.append("semgrep-sast does not take .gate-retry's retry")

    # The gates themselves.
    for name, command in sorted(CHANGE_GATES.items()):
        if name not in gates_jobs:
            problems.append(".gitlab/ci/mr-checks.yml has no %s job: a change would land without it" % name)
            continue
        text = inherited(name, gates_jobs)
        rules = rules_text(name, gates_jobs)
        for rule in GATE_RULES:
            if not has_rule(rules, rule):
                problems.append("%s does not run for every change (%s): a merge request's pipeline and a protected "
                                "batch* branch's both need it" % (name, rule))
        if re.search(r"^\s*when: never", rules, re.MULTILINE):
            problems.append("%s's rules leave it out somewhere (when: never): a blocking gate has no exception" % name)
        if re.search(r"^\s*changes:", rules, re.MULTILINE):
            problems.append("%s's rules have a changes: list: a kind of file it does not name would pass the gate unread"
                            % name)
        if "SAST_DISABLED" in rules:
            problems.append("%s's rules read SAST_DISABLED: a project variable would take a blocking gate away unseen" % name)
        if re.search(r"^\s*allow_failure:(?![ \t]*false[ \t]*$)", text, re.MULTILINE):
            problems.append("%s may fail without failing the pipeline (allow_failure): it is a blocking gate" % name)
        if re.search(r"^\s*when: manual", text, re.MULTILINE) or re.search(r"^\s*when: manual", rules, re.MULTILINE):
            problems.append("%s waits for a person (when: manual): a merge request could land past it" % name)
        if "    - " + command not in text:
            problems.append("%s does not run %s: its base is change_base's, in a merge request's pipeline and a batch* "
                            "branch's alike" % (name, command))
        block = re.search(r"^  retry:\s*\n((?:    .*\n?)+)", text, re.MULTILINE)
        block = block.group(1) if block else ""
        if (not re.search(r"^    max: 2\s*$", block, re.MULTILINE)
                or not re.search(r"^    when: !reference \[\.gate-retry, retry, when\]\s*$", block, re.MULTILINE)
                or not re.search(r"^    exit_codes:\s*\n(?:      - \d+\s*\n)*      - 2\s*$", block, re.MULTILINE)):
            problems.append("%s is not retried twice on its script's exit 2 (could not read) and on .gate-retry's reasons: "
                            "a passing hiccup of OSV, of master's fetch or of a runner would hold a change" % name)
    if not re.search(r"^    - job: %s\s*\n      artifacts: true\s*$" % GATE_SCAN, gates_jobs.get("semgrep-sast-gate", ""), re.MULTILINE):
        problems.append("semgrep-sast-gate does not need semgrep-sast with its artifacts: it would read no report")

    # The scans on linix judge a batch as a whole, against master.
    for name in WHOLE_CHANGE_SCANS:
        if BATCH_COMPARE not in rules_text(name, jobs):
            problems.append("%s does not compare a protected batch* branch with master (compare_to: refs/heads/master, then "
                            "no scan for a batch that changes documentation alone): a push of documentation on top of "
                            "code would skip it" % name)

    # build/land-via-mr.py's required jobs are jobs of the CI files, the gates and the
    # blocking scans among them, and it lets the scans off for the batches the CI files
    # run none for, and no others.
    names = set(jobs) | set(every)
    for text in texts.get("ci-includes", {}).values():
        names |= set(blocks(text))
    try:
        tree = ast.parse(texts["land"])
    except SyntaxError as error:
        tree = None
        problems.append("build/land-via-mr.py cannot be read: %s" % error)
    for constant, needed in sorted(LANDING_REQUIRED.items()):
        required = None
        for node in tree.body if tree else []:
            if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == constant for t in node.targets):
                try:
                    value = ast.literal_eval(node.value)
                    required = [str(n) for n in (value.keys() if isinstance(value, dict) else value)]
                except (ValueError, TypeError) as error:
                    problems.append("build/land-via-mr.py's %s is not a literal list of job names: %s" % (constant, error))
                    required = []
                break
        if required is None:
            if tree:
                problems.append("build/land-via-mr.py names no %s: a landing would not check the jobs it holds" % constant)
            continue
        unknown = sorted(n for n in required if n not in names)
        if unknown:
            problems.append("build/land-via-mr.py's %s requires %s, which no job of the CI files is: a landing would wait for a "
                            "job no pipeline has, or check a name that never fails" % (constant, ", ".join(unknown)))
        absent = [n for n in needed if n not in required]
        if absent:
            problems.append("build/land-via-mr.py's %s does not require %s before it lands a batch" % (constant, ", ".join(absent)))
    # Its GATE_FILES, which a batch changes only with --gates-read, name the gates' code
    # (security-gate.py's GATE_CODE, which the base's copy runs), .gitlab-ci.yml (its
    # workflow, defaults and includes), every CI file the gate jobs are in or reach with
    # extends or !reference (gate_job_files), the scripts the landing runs, and the source
    # scans' scripts (security-gate.py's SCAN_SCRIPTS); its CHECKOUT_COPIES, which it holds
    # to the base (or, on --own-copies, to the batch's tip) before it runs any, name what
    # it runs from its own checkout (LANDING_CHECKOUT); its GATE_CODE is
    # security-gate.py's; its BASE_GATES, whose logs it reads for the base, are the change
    # gates; and its BASE_COPY_SCRIPTS, whose base's copy each gate's log must say passed
    # a batch that changes GATE_CODE, name each gate's own script. No other CI file
    # defines a job that holds a change, or what every job takes (ci_override_problems),
    # and each Python script of GATE_FILES keeps its own directory off sys.path
    # (PATH_GUARD).
    landing = landing_module()
    gate_files = set(getattr(landing, "GATE_FILES", None) or ())
    needed = set(getattr(security_gate, "GATE_CODE", ())) | {".gitlab-ci.yml"} | gate_job_files(texts) | \
        set(LANDING_RUNS) | set(getattr(security_gate, "SCAN_SCRIPTS", ()))
    problems.extend(ci_override_problems(texts))
    problems.extend(path_guard_problems(gate_python_sources(landing)))
    if needed - gate_files:
        problems.append("build/land-via-mr.py's GATE_FILES leave out %s: a batch could change what judges it, or whether "
                        "and how its jobs run, with no person's word that the change was read"
                        % ", ".join(sorted(needed - gate_files)))
    copies = set(getattr(landing, "CHECKOUT_COPIES", None) or ())
    if set(LANDING_CHECKOUT) - copies:
        problems.append("build/land-via-mr.py's CHECKOUT_COPIES leave out %s: a landing would run it from a checkout whose "
                        "copy is not master's, behind it or edited, and judge the batch by that copy without a word"
                        % ", ".join(sorted(set(LANDING_CHECKOUT) - copies)))
    if security_gate is not None and sorted(getattr(landing, "GATE_CODE", None) or ()) != sorted(security_gate.GATE_CODE):
        problems.append("build/land-via-mr.py's GATE_CODE is not security-gate.py's (%s): a landing would not ask the gates' "
                        "logs for the base's copy when the batch changes the code it runs" % ", ".join(security_gate.GATE_CODE))
    if sorted(getattr(landing, "BASE_GATES", None) or ()) != sorted(CHANGE_GATES):
        problems.append("build/land-via-mr.py's BASE_GATES are not the change gates (%s): a landing would not read each "
                        "one's base from its log" % ", ".join(sorted(CHANGE_GATES)))
    scripts = dict((name, command.split()[1]) for name, command in CHANGE_GATES.items())
    if dict(getattr(landing, "BASE_COPY_SCRIPTS", None) or {}) != scripts:
        problems.append("build/land-via-mr.py's BASE_COPY_SCRIPTS are not the change gates and the scripts their jobs run "
                        "(%s): a landing would not read from each gate's log that the base's copy of its script passed a "
                        "batch that changes the gates' code" % ", ".join("%s %s" % kv for kv in sorted(scripts.items())))
    # Its documents_only, as the module has it, against the paths .own-runners' rules
    # start no scan for: the tree's files and LANDING_SAMPLE_PATHS.
    anchor = re.search(r"^        paths: &%s\s*\n((?:          - \"[^\"\n]+\"\s*\n)+)" % NOT_DOCS_ONLY, jobs.get(".own-runners", "") + "\n",
                       re.MULTILINE)
    try:
        compiled = [glob_regex(p) for p in re.findall(r'- "([^"\n]+)"', anchor.group(1))] if anchor else []
    except ValueError:
        compiled = []
    documents_only = getattr(landing_module(), LANDING_DOCUMENTS, None)
    if not callable(documents_only):
        problems.append("build/land-via-mr.py has no %s: it would require the scans of a batch the CI files run none for, "
                        "or let them off for one they run" % LANDING_DOCUMENTS)
    elif compiled:
        disagree = []
        for path in sorted(set(texts["tree"]) | set(LANDING_SAMPLE_PATHS)):
            said = documents_only([path])
            scanned = any(c.match(path) for c in compiled)
            if bool(said) == scanned:
                disagree.append("%s (%s says %r; the CI files %s it)" % (path, LANDING_DOCUMENTS, said,
                                                                       "scan" if scanned else "do not scan"))
        if disagree:
            problems.append("build/land-via-mr.py's %s and .own-runners' paths: *%s disagree on %d path(s), so a landing "
                            "would wait for a scan no pipeline ran or let off one that ran: %s" % (
                                LANDING_DOCUMENTS, NOT_DOCS_ONLY, len(disagree), "; ".join(disagree[:6])))
    return problems, notes


def ci_files(texts):
    """{path: text} of every CI file: each of FILES as texts holds it, so that the
    self-test's breakings are read, and the other included files as the tree has them."""
    every_ci = dict(texts.get("ci-includes", {}))
    for key, relative in FILES.items():
        if relative.replace(os.sep, "/") in every_ci and key in texts:
            every_ci[relative.replace(os.sep, "/")] = texts[key]
    every_ci[".gitlab-ci.yml"] = texts["ci"]
    return every_ci


# A key at the top level of a CI file, a job's or a template's name among them, in any of
# the forms YAML takes for one: bare or quoted, with its value on the line or below it.
TOP_LEVEL_KEY = re.compile(r"^(?:\"([^\"\n]+)\"|'([^'\n]+)'|([A-Za-z0-9_.\-]+))[ \t]*:(?:[ \t]|$)", re.MULTILINE)
# The jobs whose definitions say whether and how a change is held: the change gates and
# their scan, the blocking scans on linix, and checkers, which holds the CI files to them.
HELD_JOBS = sorted(CHANGE_GATES) + [GATE_SCAN] + BLOCKING_SCANS + ["checkers"]
# The top-level keys every job takes from the file that sets them: .gitlab-ci.yml's alone,
# a file of build/land-via-mr.py's GATE_FILES.
EVERY_JOB_KEYS = ("variables", "default", "workflow")


def top_level_keys(text):
    """The keys at the top level of a CI file's text, in order."""
    return [m.group(1) or m.group(2) or m.group(3) for m in TOP_LEVEL_KEY.finditer((text or "").replace("\r\n", "\n"))]


def reached_definitions(every_ci, names):
    """{name: [the CI files that define it]} for each of names and every template they
    extend or take a part from with !reference, at any remove, through every definition
    of each."""
    defined = {}
    for path in sorted(every_ci):
        bodies = blocks(every_ci[path] or "")
        for name in top_level_keys(every_ci[path]):
            defined.setdefault(name, []).append((path, bodies.get(name, "")))
    found, pending = {}, list(names)
    while pending:
        name = pending.pop()
        if name in found or name not in defined:
            continue
        found[name] = sorted(set(path for path, _ in defined[name]))
        for _, body in defined[name]:
            pending.extend(re.findall(r"!reference \[\s*([^,\]\s]+)", body))
            extends = re.search(r"^  extends:[ \t]*(\S[^\n]*)?\n?((?:    - \S+[ \t]*\n?)*)", body, re.MULTILINE)
            if extends:
                pending.extend(re.findall(r"[^\s,\[\]'\"]+", extends.group(1) or ""))
                pending.extend(re.findall(r"^    - (\S+)", extends.group(2) or "", re.MULTILINE))
    return found


def gate_job_files(texts):
    """The CI files that say whether and how the gate jobs run (CHANGE_GATES and
    GATE_SCAN): the ones they are defined in, and the ones every template they extend or
    take a part from with !reference is defined in, at any remove."""
    return set(path for paths in reached_definitions(ci_files(texts), sorted(CHANGE_GATES) + [GATE_SCAN]).values()
               for path in paths)


def ci_override_problems(texts):
    """[problem] for each way a CI file other than a job's own would say whether and how
    a job of HELD_JOBS runs: GitLab merges every definition of a name, the keys of a later
    include over an earlier one's and .gitlab-ci.yml's over all, so a second definition of
    one of them, or of a template it reaches, in a file a landing asks no person's word
    for, could take the job's rules, script or allow_failure; a file but .gitlab-ci.yml
    that sets what every job takes (EVERY_JOB_KEYS); and an include in one that brings in
    a file this cannot read (any but one of GitLab's templates)."""
    every_ci = ci_files(texts)
    problems = []
    for name, paths in sorted(reached_definitions(every_ci, HELD_JOBS).items()):
        if len(paths) > 1:
            problems.append("%s is defined in %s: GitLab merges the definitions, a later include's keys over an earlier "
                            "one's, so a file other than its own would say whether and how a job that holds a change "
                            "runs - define it once" % (name, " and ".join(paths)))
    for path, text in sorted(every_ci.items()):
        if path == ".gitlab-ci.yml":
            continue
        keys = top_level_keys(text)
        global_keys = [k for k in keys if k in EVERY_JOB_KEYS]
        if global_keys:
            problems.append("%s sets %s at its top level, which every job takes, the gates among them: only .gitlab-ci.yml "
                            "sets that" % (path, ", ".join(global_keys)))
        if "include" not in keys:
            continue
        included = blocks(text).get("include")
        other = [line.strip() for line in (included if included is not None else "include: on one line").split("\n")
                 if line.strip() and not re.match(r"^\s*-\s*template\s*:\s*\S+\s*$", line)]
        if other:
            problems.append("%s includes what is not one of GitLab's templates, each as - template: <name> (%s): a "
                            "definition there of a job that holds a change could not be read here - include it from "
                            ".gitlab-ci.yml" % (path, other[0][:80]))
    return problems


# ---------------------------------------------------------------- the runners
#
# Every job of the CI files runs on the project's own runners in a pipeline for a
# protected ref that is not a merge request's (docs/ContinuousIntegration.md, "Every job
# on the project's own runners"): GitLab's compute minutes ran out on 9 October 2026, and
# batch46's pipeline 2931387790 could not start semgrep-sast-gate, the one blocking job
# left on GitLab's shared runners (stuck_pending_no_matching_runners, then
# ci_quota_exceeded). The runners as GitLab has them (glab api projects/86730559/runners,
# runners/<id>; 9 October 2026): each a project runner, ref_protected, run_untagged off,
# locked. A job reaches one only when every tag it names is one of that runner's; and one
# of them must be a tag GitLab's hosted runners do not carry, which the runners' names
# are (linux, docker and windows are generic words a runner anywhere may carry). s1-linux,
# a virtual machine on server1, carries linix's tags since 13 October 2026, and server2
# carries kvm beside them, which the owner adds (runner 57072507's tags): its machine has
# /dev/kvm, and linix and s1-linux, virtual machines themselves, have none (KVM_JOBS).
PROJECT_RUNNERS = (("linix (56743206) and s1-linux (57371020)", ("linux", "docker", "linix")),
                   ("server2 (57072507)", ("linux", "docker", "linix", "kvm")),
                   ("linix-systemd (56932743)", ("linix-systemd",)),
                   ("bench150 (56743216)", ("windows", "bench", "bench150")))
PROJECT_ONLY_TAGS = ("linix", "linix-systemd", "bench150")
# The jobs that name no runner of their own take default:'s tags, the one variable below:
# linix in .gitlab-ci.yml's variables:, and GitLab's hosted small Linux runner's tag where
# a workflow rule sets it - for every merge request's pipeline and every pipeline of an
# unprotected ref, which the project's runners take nothing of (a fork's runs in the
# fork, on the fork's runners).
RUNNER_VARIABLE = "HM_RUNNER"
DEFAULT_RUNNER = "linix"
HOSTED_RUNNER = "saas-linux-small-amd64"
# GitLab's hosted runners beside the project's own (docs/ContinuousIntegration.md,
# "GitLab's hosted runners beside the project's"). A class of the jobs on the project's
# runners that need nothing of the project's takes its tags from a switch: a variable
# that .gitlab-ci.yml's variables: set to linix and nothing else in the CI files sets,
# which a protected project variable of the same name - the owner's - sets to one of
# GitLab's hosted Linux machines of the class's architecture, in a pipeline of a
# protected ref alone (a merge request's has none of these jobs, and no protected
# variable). A class whose jobs wait for each other on the project's machines takes its
# resource group from a lane, a variable set there to the group's name, which the owner
# sets to hosted-$CI_JOB_NAME beside the switch, a lane of each job's own. Each class
# names its jobs here, so that a job joins one, or leaves it, by an edit here as well.
_DEB_AMD64 = ("debian12", "debian13", "ubuntu24.04")
_DEB_ARM64 = ("debian12", "debian13", "ubuntu24.04", "ubuntu26.04")
_RPM_TARGETS = ("fedora44", "fedora43", "el10", "el9", "leap16", "tumbleweed")
_SRCPKG_TARGETS = ("debian13", "ubuntu24.04", "ubuntu26.04", "fedora44", "el10", "leap16", "tumbleweed")
RUNNER_CLASSES = {
    # The builds and checks in the CI image or in a distribution's own amd64 image.
    "HM_RUNNER_BUILD": {"arch": "amd64", "lane": None, "jobs": (
        "linux-build", "linux-build-gcc", "linux-cross", "linux-sanitize", "linux-repro", "linux-sbom", "linux-live-update",
        "linux-db-upgrade-pgsql", "linux-db-upgrade-mysql", "linux-package-deb", "linux-package-rpm", "linux-image",
        "static-analysis", "semgrep", "ci-image-packages", "dependency-scan", "repo-hygiene", "source-tarball",
        "release-packages", "deb-ubuntu26.04-arm64-sbom", "rpm-fedora44-aarch64-sbom")},
    # The package builds and their install jobs in amd64 containers: every .deb, the
    # arm64 ones cross-compiled; the x86_64 .rpm files, since an aarch64 one's build runs
    # its testers under qemu-user, which GitLab's amd64 machines have not; the source
    # packages; Alpine's; the AppImages' amd64 jobs; and the amd64 snap.
    "HM_RUNNER_PACKAGES": {"arch": "amd64", "lane": ("HM_LANE_PACKAGES", "linix-package-build"), "jobs": tuple(
        ["deb-%s-amd64" % d for d in _DEB_AMD64] + ["deb-%s-arm64" % d for d in _DEB_ARM64] +
        ["deb-%s-amd64-install" % d for d in _DEB_AMD64] +
        ["rpm-%s-x86_64" % t for t in _RPM_TARGETS] + ["rpm-%s-x86_64-install" % t for t in _RPM_TARGETS] +
        ["srcpkg-%s" % t for t in _SRCPKG_TARGETS] + ["srcpkg-%s-install" % t for t in _SRCPKG_TARGETS] +
        ["alpine-package", "alpine-install", "appimage-runtime-x86_64", "appimage-x86_64", "appimage-test-x86_64",
         "appimage-runtime-sources", "appimage-bundled-sources", "appimage-aarch64-compile", "snap-build-amd64"])},
    # The jobs in an arm64 container, which linix and server2 run under qemu-user.
    "HM_RUNNER_ARM64": {"arch": "arm64", "lane": None, "jobs": tuple(
        ["deb-%s-arm64-install" % d for d in _DEB_ARM64] + ["rpm-%s-aarch64-install" % t for t in _RPM_TARGETS] +
        ["appimage-runtime-aarch64", "appimage-aarch64", "appimage-test-aarch64", "snap-build-arm64", "linux-image-arm64"])},
    # The two whole suites.
    "HM_RUNNER_SUITE": {"arch": "amd64", "lane": ("HM_LANE_SUITE", "linix-suite"), "jobs": ("linux-suite", "alpine-suite")},
}
# Each switch and lane by its default in .gitlab-ci.yml's variables:.
SWITCH_DEFAULTS = dict([(variable, DEFAULT_RUNNER) for variable in RUNNER_CLASSES] +
                       [spec["lane"] for spec in RUNNER_CLASSES.values() if spec["lane"]])
# GitLab's hosted Linux machines (docs.gitlab.com, "Hosted runners on Linux"), by
# architecture: the tags a switch may name.
HOSTED_MACHINES = {
    "amd64": ("saas-linux-small-amd64", "saas-linux-medium-amd64", "saas-linux-large-amd64", "saas-linux-xlarge-amd64",
              "saas-linux-2xlarge-amd64"),
    "arm64": ("saas-linux-small-arm64", "saas-linux-medium-arm64", "saas-linux-large-arm64"),
}
# The checks of a pipeline a switch is set for, each by its subcommand of HOSTED_SCRIPT;
# compute-quota alone names an environment, the one its token is scoped to.
HOSTED_CHECKS_FILE = ".gitlab/ci/hosted-runners.yml"
HOSTED_SCRIPT = os.path.join("build", "ci", "hosted-runners.py")
HOSTED_CHECKS = {"hosted-runners": "check", "compute-quota": "quota"}
HOSTED_QUOTA_ENVIRONMENT = "compute-quota"
# The jobs that boot a QEMU machine, which wants KVM, and the tags that put each on
# server2, the one runner of the project's whose machine has it.
KVM_JOBS = {"nix": ["linix", "kvm"]}
# CI_PIPELINE_SOURCE's values (GitLab's predefined variables), and one for any other.
PIPELINE_SOURCES = ("push", "web", "api", "trigger", "schedule", "pipeline", "parent_pipeline", "merge_request_event",
                    "external", "external_pull_request_event", "chat", "webide", "ondemand_dast_scan",
                    "security_orchestration_policy", "any-other")
# The two variables a pipeline's kind is told by here: whether its ref is protected, and
# whether it is a merge request's. Every other clause of a rules:if may be either.
KIND_VARIABLES = ("CI_COMMIT_REF_PROTECTED", "CI_PIPELINE_SOURCE")
# How many clauses of other kinds a rules list may have: each is tried both ways.
MOST_OPEN_CLAUSES = 14

CONDITION_TOKEN = re.compile(r"\s*(?:(\$\{?[A-Za-z_][A-Za-z0-9_]*\}?)|(\"(?:[^\"\\\n]|\\.)*\"|'(?:[^'\\\n]|\\.)*')|"
                             r"(/(?:[^/\\\n]|\\.)*/[a-z]*)|(==|!=|=~|!~|&&|\|\||\(|\))|(null)(?![A-Za-z0-9_]))")


def condition_tree(expr):
    """A rules:if expression as a tree: ("or", a, b), ("and", a, b), or ("clause", c),
    where c is ("kind", variable, operator, value) for CI_COMMIT_REF_PROTECTED or
    CI_PIPELINE_SOURCE held to a literal (or asked whether it is set, operator "set"),
    which the pipeline's kind decides, and ("open", text) for any other clause, which may
    be either. ValueError for what this does not read."""
    tokens, i = [], 0
    while i < len(expr):
        if expr[i:].strip() == "":
            break
        m = CONDITION_TOKEN.match(expr, i)
        if not m or m.end() == i:
            raise ValueError("cannot read %r at %r" % (expr, expr[i:i + 20]))
        var, string, regex, op, null = m.groups()
        if var:
            tokens.append(("var", var.strip("${}")))
        elif string:
            tokens.append(("str", re.sub(r"\\(.)", r"\1", string[1:-1])))
        elif regex:
            tokens.append(("re", regex))
        elif null:
            tokens.append(("null", None))
        else:
            tokens.append(("op", op))
        i = m.end()
    at = [0]

    def peek():
        return tokens[at[0]] if at[0] < len(tokens) else None

    def take(kind=None):
        if at[0] >= len(tokens):
            raise ValueError("%r ends too soon" % expr)
        token = tokens[at[0]]
        if kind and token[0] != kind:
            raise ValueError("%r: %r where %s was expected" % (expr, token, kind))
        at[0] += 1
        return token

    def spelled(token):
        kind, value = token
        return {"var": "$%s" % value, "str": '"%s"' % value, "re": value, "null": "null"}[kind]

    def clause():
        token = take()
        if token == ("op", "("):
            tree = alternatives()
            if take() != ("op", ")"):
                raise ValueError("%r: unbalanced parentheses" % expr)
            return tree
        if token[0] == "op":
            raise ValueError("%r: %s where a value was expected" % (expr, token[1]))
        following = peek()
        if following and following[0] == "op" and following[1] in ("==", "!=", "=~", "!~"):
            op = take()[1]
            other = take()
            if other[0] == "op":
                raise ValueError("%r: %s where a value was expected" % (expr, other[1]))
            left, right = token, other
            if right[0] == "var" and right[1] in KIND_VARIABLES and left[0] in ("str", "null") and op in ("==", "!="):
                left, right = right, left
            if left[0] == "var" and left[1] in KIND_VARIABLES and right[0] in ("str", "null", "re"):
                if op in ("=~", "!~") and right[0] != "re":
                    raise ValueError("%r: %s against %s" % (expr, op, spelled(right)))
                return ("clause", ("kind", left[1], op, right[1]))
            return ("clause", ("open", "%s %s %s" % (spelled(left), op, spelled(right))))
        if token[0] == "var" and token[1] in KIND_VARIABLES:
            return ("clause", ("kind", token[1], "set", None))
        return ("clause", ("open", spelled(token)))

    def conjunction():
        tree = clause()
        while peek() == ("op", "&&"):
            take()
            tree = ("and", tree, clause())
        return tree

    def alternatives():
        tree = conjunction()
        while peek() == ("op", "||"):
            take()
            tree = ("or", tree, conjunction())
        return tree

    if not tokens:
        raise ValueError("an empty condition")
    tree = alternatives()
    if at[0] != len(tokens):
        raise ValueError("%r: %r after the end" % (expr, tokens[at[0]]))
    return tree


def open_clauses(tree, found=None):
    """The texts of a tree's open clauses, in order, each once."""
    found = [] if found is None else found
    if tree[0] in ("or", "and"):
        open_clauses(tree[1], found)
        open_clauses(tree[2], found)
    elif tree[1][0] == "open" and tree[1][1] not in found:
        found.append(tree[1][1])
    return found


def condition_holds(tree, kind, chosen):
    """Whether a tree holds for a pipeline of kind ({variable: value}) with its open
    clauses as chosen ({text: bool})."""
    if tree[0] == "or":
        return condition_holds(tree[1], kind, chosen) or condition_holds(tree[2], kind, chosen)
    if tree[0] == "and":
        return condition_holds(tree[1], kind, chosen) and condition_holds(tree[2], kind, chosen)
    c = tree[1]
    if c[0] == "open":
        return chosen[c[1]]
    _, variable, op, value = c
    actual = kind.get(variable)
    if op == "set":
        return bool(actual)
    if op in ("==", "!="):
        same = actual is None if value is None else actual == value
        return same if op == "==" else not same
    body, flags = value[1:value.rindex("/")], value[value.rindex("/") + 1:]
    found = actual is not None and re.search(body, actual, re.IGNORECASE if "i" in flags else 0) is not None
    return found if op == "=~" else not found


def yaml_scalar(text):
    """A one-line YAML scalar's value: quoted, or plain with a comment cut off."""
    text = text.strip()
    if len(text) > 1 and text[0] == text[-1] == "'":
        return text[1:-1].replace("''", "'")
    if len(text) > 1 and text[0] == text[-1] == '"':
        return re.sub(r"\\(.)", r"\1", text[1:-1])
    return re.split(r"\s#", text, maxsplit=1)[0].strip()


def parsed_rule(item, where):
    """One item of a rules list (rule_items' text) as {"if": tree or None, "when",
    "runner": the HM_RUNNER its variables set or None, "narrowed": whether it has
    changes: or exists:, which may be either}. ValueError for what this does not read."""
    lines = item.rstrip("\n").split("\n")
    keys = [(lines[0], [])]
    for line in lines[1:]:
        if not line.strip():
            continue
        if re.match(r"^      [A-Za-z_]+:", line):
            keys.append((line.strip(), []))
        elif line.startswith("       "):
            keys[-1][1].append(line.strip())
        else:
            raise ValueError("%s: a rule written as %r" % (where, line.strip()))
    rule = {"if": None, "when": "on_success", "runner": None, "narrowed": False, "text": lines[0].strip()}
    for first, deeper in keys:
        key, _, value = first.partition(":")
        key = key.strip()
        if key == "if":
            if not value.strip():
                raise ValueError("%s: an if: this does not read" % where)
            rule["if"] = condition_tree(" ".join([yaml_scalar(value)] + [yaml_scalar(d) for d in deeper]))
        elif key == "when":
            rule["when"] = yaml_scalar(value)
        elif key in ("changes", "exists"):
            rule["narrowed"] = True
        elif key == "variables":
            for line in deeper:
                m = re.match(r"""^["']?%s["']?\s*:\s*(.+)$""" % RUNNER_VARIABLE, line)
                if m:
                    rule["runner"] = yaml_scalar(m.group(1))
            if value.strip():
                raise ValueError("%s: variables: written on one line" % where)
        elif value.strip().startswith(("*", "&", "<<")) or key == "<<":
            raise ValueError("%s: %s by an alias or a merge key" % (where, key))
    return rule


def rule_list(name, jobs, where, seen=None):
    """The rules a block's rules: key holds, each !reference [X, rules] replaced by X's,
    parsed; None when it has no rules: key. ValueError for what this does not read."""
    seen = set() if seen is None else seen
    text = jobs.get(name, "")
    head = re.search(r"^  rules:(.*)$", text, re.MULTILINE)
    if not head:
        return None
    if head.group(1).strip():
        raise ValueError("%s's rules are written on one line (%s)" % (name, head.group(1).strip()[:40]))
    body = re.search(r"^  rules:\s*\n(.*?)(?=^  [A-Za-z_]|\Z)", text, re.MULTILINE | re.DOTALL).group(1)
    if body.strip() and not body.lstrip("\n").startswith("    - "):
        raise ValueError("%s's rules are not a list indented four" % name)
    rules = []
    for item in re.split(r"^    - ", body, flags=re.MULTILINE)[1:]:
        reference = re.match(r"^!reference \[\s*([^,\]\s]+)\s*,\s*rules\s*\]\s*$", item.strip())
        if reference:
            other = reference.group(1)
            if other in seen or other not in jobs:
                raise ValueError("%s takes !reference [%s, rules], which %s" % (
                    name, other, "takes it back" if other in seen else "no CI file defines"))
            found = rule_list(other, jobs, where, seen | {name})
            if found is None:
                raise ValueError("%s takes !reference [%s, rules], which has no rules" % (name, other))
            rules += found
        elif item.strip().startswith("!reference"):
            raise ValueError("%s's rules take %s" % (name, item.strip()[:60]))
        else:
            rules.append(parsed_rule(item, "%s in %s" % (name, where)))
    return rules


def rules_decide(rules, kind, chosen, narrowed):
    """The rule that decides for a pipeline of kind, its open clauses as chosen and each
    rule's changes:/exists: as narrowed ({index: bool}): (index, rule), or None when none
    matches."""
    for index, rule in enumerate(rules):
        if rule["if"] is not None and not condition_holds(rule["if"], kind, chosen):
            continue
        if rule["narrowed"] and not narrowed[index]:
            continue
        return index, rule
    return None


def pipeline_kinds():
    """Every kind of pipeline, as its two variables, with whether the project's runners
    may take its jobs: a protected ref's that is not a merge request's."""
    for protected in ("true", "false"):
        for source in PIPELINE_SOURCES:
            yield ({"CI_COMMIT_REF_PROTECTED": protected, "CI_PIPELINE_SOURCE": source},
                   protected == "true" and source != "merge_request_event")


def ways(rules):
    """Every way a rules list can go: (chosen, narrowed) for each choice of its open
    clauses and its changes:/exists: rules. ValueError past MOST_OPEN_CLAUSES."""
    clauses = []
    for rule in rules:
        if rule["if"] is not None:
            open_clauses(rule["if"], clauses)
    narrowing = [i for i, rule in enumerate(rules) if rule["narrowed"]]
    if len(clauses) + len(narrowing) > MOST_OPEN_CLAUSES:
        raise ValueError("%d clauses and changes: to try both ways, more than %d" % (
            len(clauses) + len(narrowing), MOST_OPEN_CLAUSES))
    for bits in range(2 ** (len(clauses) + len(narrowing))):
        chosen = dict((c, bool(bits >> n & 1)) for n, c in enumerate(clauses))
        narrowed = dict((i, bool(bits >> (len(clauses) + n) & 1)) for n, i in enumerate(narrowing))
        yield chosen, narrowed


def tag_values(block):
    """The tags a block names, as a list; None when it has no tags: key. ValueError for a
    form this does not read (an alias, a reference, a mapping)."""
    head = re.search(r"^  tags:[ \t]*(.*?)[ \t]*$", block, re.MULTILINE)
    if not head:
        return None
    inline = re.split(r"\s#", head.group(1), maxsplit=1)[0].strip()
    if inline:
        if inline.startswith("[") and inline.endswith("]") and "[" not in inline[1:]:
            return [v.strip().strip("'\"") for v in inline[1:-1].split(",") if v.strip()]
        raise ValueError("tags: %s" % inline[:40])
    found = []
    for line in block[head.end():].split("\n")[1:]:
        if not line.strip():
            continue
        item = re.match(r"^    - (.+)$", line)
        if not item:
            break
        value = yaml_scalar(item.group(1))
        if value.startswith(("*", "&", "[", "{", "!")):
            raise ValueError("tags: - %s" % value)
        found.append(value)
    return found


def runner_problems(texts):
    """([problem], [note]) for each job of the CI files that could run on a runner that is
    not the project's, or wait for one that never takes it:

      * .gitlab-ci.yml's default: must name the tags [$HM_RUNNER], which every job that
        names none of its own - GitLab's templates' jobs among them - takes, and its
        variables: must set HM_RUNNER to linix; nothing else in the CI files may set
        HM_RUNNER but a workflow rule; no job may leave default:'s keys (inherit:);
      * the workflow's rules, tried for every kind of pipeline and every way their other
        clauses can go, must leave HM_RUNNER linix (or another tag of the project's
        alone) for every pipeline they make of a protected ref that is not a merge
        request's, and set it to GitLab's hosted runner's tag for every other one they
        make, a merge request's or an unprotected ref's, which the project's runners -
        registered ref_protected - take nothing of;
      * a job that names its own tags must name a runner of the project's: every tag one
        runner's, and one of them a tag only the project's carry (PROJECT_ONLY_TAGS) -
        not none, not a variable but HM_RUNNER, not GitLab's - and then its rules may
        create it in no pipeline the workflow makes for a merge request or an unprotected
        ref, where it would wait for a runner that never takes it.
    What this cannot read is a problem."""
    problems, notes = [], []
    every_ci = ci_files(texts)
    ci_blocks = blocks(texts["ci"])
    order = [p for p in sorted(every_ci) if p != ".gitlab-ci.yml"] + [".gitlab-ci.yml"]
    definitions = {}
    for path in order:
        bodies = blocks(every_ci[path] or "")
        for name in top_level_keys(every_ci[path]):
            if name in RESERVED:
                continue
            if name not in bodies:
                problems.append("%s in %s is not written as a block this reads (a value on the key's own line, or a quoted "
                                "key): its runner cannot be told" % (name, path))
                continue
            definitions.setdefault(name, []).append((path, bodies[name]))
    # One block per name, as templates are read (the last definition, .gitlab-ci.yml's
    # over an include's); each definition's tags are read below.
    jobs = dict((name, defs[-1][1]) for name, defs in definitions.items())

    default = ci_blocks.get("default", "")
    try:
        default_tags = tag_values(default)
    except ValueError as error:
        default_tags = None
        problems.append(".gitlab-ci.yml's default: names its tags in a form this does not read (%s)" % error)
    wanted = ["$%s" % RUNNER_VARIABLE]
    if default_tags != wanted:
        problems.append(".gitlab-ci.yml's default: names %s, not tags: [$%s]: a job that names no runner of its own - "
                        "GitLab's templates' among them - would run %s" % (
                            "no tags" if default_tags is None else "the tags %s" % default_tags, RUNNER_VARIABLE,
                            "on GitLab's shared runners, whose compute minutes ran out on 9 October 2026"
                            if not default_tags else "where nothing here holds it"))
    variables = ci_blocks.get("variables", "")
    set_to = re.findall(r"""^  ["']?%s["']?[ \t]*:[ \t]*(.*?)[ \t]*$""" % RUNNER_VARIABLE, variables, re.MULTILINE)
    global_runner = yaml_scalar(set_to[0]) if len(set_to) == 1 else None
    if global_runner != DEFAULT_RUNNER:
        problems.append(".gitlab-ci.yml's variables: %s %s: it must be %s, the tag linix and server2 carry, for every "
                        "pipeline of a protected ref that is not a merge request's" % (
                            "set" if set_to else "do not set", RUNNER_VARIABLE if not set_to else "%s to %r" % (
                                RUNNER_VARIABLE, global_runner), DEFAULT_RUNNER))
    for path in order:
        top = None
        for line in (every_ci[path] or "").replace("\r\n", "\n").split("\n"):
            if line[:1] not in ("", " ", "\t", "#"):
                top = line.split(":", 1)[0].strip().strip("'\"")
            code = re.split(r"(?:^|\s)#", line, maxsplit=1)[0]
            if not re.search(r"""(?:^|[\s{,\[])["']?%s["']?[ \t]*:""" % RUNNER_VARIABLE, code):
                continue
            allowed = path == ".gitlab-ci.yml" and ((top == "variables" and re.match(r"^  %s:" % RUNNER_VARIABLE, line)) or
                                                    (top == "workflow" and re.match(r"^        %s:" % RUNNER_VARIABLE, line)))
            if not allowed:
                problems.append("%s sets %s in %s (%s): only .gitlab-ci.yml's variables: and a workflow rule may, or a "
                                "job could take default:'s tags to a runner nothing here holds it to" % (
                                    path, RUNNER_VARIABLE, top, line.strip()[:60]))
    # The switches and the lanes: .gitlab-ci.yml's variables: alone set them, each to its
    # default, the project's runners'; the owner's project variable, which stands above,
    # moves a class (RUNNER_CLASSES).
    for variable, default in sorted(SWITCH_DEFAULTS.items()):
        set_to = re.findall(r"""^  ["']?%s["']?[ \t]*:[ \t]*(.*?)[ \t]*$""" % variable, variables, re.MULTILINE)
        value = yaml_scalar(set_to[0]) if len(set_to) == 1 else None
        if value != default:
            problems.append(".gitlab-ci.yml's variables: %s: it must be %s, so that its class's jobs run where they always "
                            "have unless the owner's project variable %s says otherwise" % (
                                "do not set %s" % variable if not set_to else "set %s to %r" % (variable, value), default,
                                variable))
    for path in order:
        top = None
        for line in (every_ci[path] or "").replace("\r\n", "\n").split("\n"):
            if line[:1] not in ("", " ", "\t", "#"):
                top = line.split(":", 1)[0].strip().strip("'\"")
            code = re.split(r"(?:^|\s)#", line, maxsplit=1)[0]
            for variable in sorted(SWITCH_DEFAULTS):
                if not re.search(r"""(?:^|[\s{,\[])["']?%s["']?[ \t]*:""" % variable, code):
                    continue
                if path == ".gitlab-ci.yml" and top == "variables" and re.match(r"^  %s:" % variable, line):
                    continue
                problems.append("%s sets %s in %s (%s): only .gitlab-ci.yml's variables: may, at its default, so that the "
                                "owner's project variable alone moves its class" % (path, variable, top, line.strip()[:60]))
    for name, defs in sorted(definitions.items()):
        for path, body in defs:
            if re.search(r"^  inherit:", body, re.MULTILINE):
                problems.append("%s in %s says inherit:, which can leave default:'s tags behind: a job names its runner, "
                                "or takes default:'s" % (name, path))
            if re.search(r"^  extends:[ \t]*$", body, re.MULTILINE):
                problems.append("%s in %s gives extends: as a block list, which this does not follow to the tags and rules "
                                "it takes: extends: [a, b]" % (name, path))

    # The workflow: which runner each pipeline it makes is given.
    admitted_elsewhere = []
    try:
        workflow = rule_list("workflow", {"workflow": ci_blocks.get("workflow", "")}, ".gitlab-ci.yml")
        if workflow is None:
            raise ValueError("it has no rules")
        if any(r["when"] not in ("always", "never") and r["when"] != "on_success" for r in workflow):
            raise ValueError("a rule says when: %s" % [r["when"] for r in workflow if r["when"] not in ("always", "never", "on_success")][0])
        wrong = {}
        for kind, ours in pipeline_kinds():
            for chosen, narrowed in ways(workflow):
                decided = rules_decide(workflow, kind, chosen, narrowed)
                if decided is None or decided[1]["when"] == "never":
                    continue
                index, rule = decided
                runner = rule["runner"] or global_runner
                if not ours and kind not in admitted_elsewhere:
                    admitted_elsewhere.append(kind)
                if ours and runner not in PROJECT_ONLY_TAGS:
                    wrong.setdefault((index, True, runner), (rule, kind))
                elif not ours and runner != HOSTED_RUNNER:
                    wrong.setdefault((index, False, runner), (rule, kind))
        for (index, ours, runner), (rule, kind) in sorted(wrong.items(), key=lambda item: (item[0][0], item[0][1], str(item[0][2]))):
            if ours:
                problems.append("the workflow's rule %d (%s) makes a pipeline for a protected ref that is not a merge "
                                "request's (CI_PIPELINE_SOURCE %s, among others) with %s %r: its jobs that name no runner "
                                "would go %s, where every job of such a pipeline runs on the project's own" % (
                                    index + 1, rule["text"][:90], kind["CI_PIPELINE_SOURCE"], RUNNER_VARIABLE, runner,
                                    "to GitLab's hosted runners" if runner == HOSTED_RUNNER else "where no runner of the project's is"))
            else:
                problems.append("the workflow's rule %d (%s) makes a pipeline %s (CI_COMMIT_REF_PROTECTED %s, "
                                "CI_PIPELINE_SOURCE %s, among others) with %s %r: the project's runners, registered "
                                "ref_protected, take nothing of it, so its jobs would wait for a runner until GitLab fails "
                                "them; such a rule sets %s: %s, GitLab's hosted runner" % (
                                    index + 1, rule["text"][:90],
                                    "for a merge request" if kind["CI_PIPELINE_SOURCE"] == "merge_request_event" else
                                    "for an unprotected ref", kind["CI_COMMIT_REF_PROTECTED"], kind["CI_PIPELINE_SOURCE"],
                                    RUNNER_VARIABLE, runner, RUNNER_VARIABLE, HOSTED_RUNNER))
    except ValueError as error:
        problems.append(".gitlab-ci.yml's workflow: is not read here (%s): which runner its pipelines' jobs go to cannot "
                        "be told" % error)

    # The jobs.
    chosen_runner, own, classed = [], [], {}
    for name in sorted(definitions):
        if name.startswith("."):
            continue
        where = " and ".join(sorted(set(path for path, _ in definitions[name])))
        named = []
        try:
            for path, body in definitions[name]:
                found = tag_values(body)
                if found is not None:
                    named.append((path, found))
            if len(named) > 1:
                problems.append("%s names its tags in %s: GitLab merges the definitions, and one runner is named once"
                                % (name, " and ".join(path for path, _ in named)))
                continue
            tags = named[0][1] if named else None
            if tags is None:
                for current in template_order(name, jobs)[1:]:
                    tags = tag_values(jobs[current])
                    if tags is not None:
                        break
        except ValueError as error:
            problems.append("%s in %s names its tags in a form this does not read (%s): its runner cannot be told"
                            % (name, where, error))
            continue
        if tags is None:
            tags = default_tags if default_tags is not None else []
            if tags == wanted:
                chosen_runner.append(name)
                continue
        if tags == wanted:
            chosen_runner.append(name)
            continue
        if not tags:
            problems.append("%s (%s) names no tags: GitLab's shared runners would take it, whose compute minutes ran out "
                            "on 9 October 2026 - a job takes default:'s [$%s] or names a runner of the project's"
                            % (name, where, RUNNER_VARIABLE))
            continue
        if len(tags) == 1 and tags[0].startswith("$") and tags[0][1:] in RUNNER_CLASSES:
            variable = tags[0][1:]
            if name not in RUNNER_CLASSES[variable]["jobs"]:
                problems.append("%s (%s) takes its tags from %s, and that class does not name it (RUNNER_CLASSES): a job goes "
                                "to GitLab's runners with a class only once it is known to need nothing of the project's"
                                % (name, where, variable))
                continue
            classed.setdefault(variable, []).append(name)
            # Its rules are held as a job's on linix, where its switch leaves it: for a
            # protected ref that is not a merge request's alone.
            tags = [DEFAULT_RUNNER]
        if any("$" in t for t in tags):
            problems.append("%s (%s) names its tags %s by a variable other than %s alone, or a class's switch alone: where it "
                            "runs cannot be told" % (name, where, tags, RUNNER_VARIABLE))
            continue
        if not any(set(tags) <= set(carried) for _, carried in PROJECT_RUNNERS):
            problems.append("%s (%s) names the tags %s, which no runner of the project's carries all of (%s): it would "
                            "wait for a runner, or go to one that is not the project's" % (
                                name, where, tags, "; ".join("%s: %s" % (who, ", ".join(c)) for who, c in PROJECT_RUNNERS)))
            continue
        if not set(tags) & set(PROJECT_ONLY_TAGS):
            problems.append("%s (%s) names the tags %s, none of which only the project's runners carry: a runner that is "
                            "not the project's may carry them too - it names one of %s" % (
                                name, where, tags, ", ".join(PROJECT_ONLY_TAGS)))
            continue
        own.append(name)
        try:
            rules = None
            for current in template_order(name, jobs):
                rules = rule_list(current, jobs, where)
                if rules is not None:
                    break
            if rules is None:
                rules = [{"if": None, "when": "on_success", "runner": None, "narrowed": False, "text": "(no rules)"}]
            stranded = None
            for kind in admitted_elsewhere:
                for chosen, narrowed in ways(rules):
                    decided = rules_decide(rules, kind, chosen, narrowed)
                    if decided is not None and decided[1]["when"] != "never":
                        stranded = (kind, decided)
                        break
                if stranded:
                    break
        except ValueError as error:
            problems.append("%s (%s) is tagged %s, for a runner of the project's, and its rules are not read here (%s): "
                            "whether a merge request's or an unprotected ref's pipeline can make it cannot be told"
                            % (name, where, tags, error))
            continue
        if stranded:
            kind, (index, rule) = stranded
            problems.append("%s (%s) is tagged %s, for a runner of the project's, and its rule %d (%s) makes it in a "
                            "pipeline %s (CI_COMMIT_REF_PROTECTED %s, CI_PIPELINE_SOURCE %s) that the workflow makes: "
                            "the project's runners, registered ref_protected, take nothing of that pipeline, so it would "
                            "wait for a runner until GitLab fails it - a job tagged for them asks for a protected ref "
                            "that is not a merge request's, or takes default:'s tags" % (
                                name, where, tags, index + 1, rule["text"][:90],
                                "for a merge request" if kind["CI_PIPELINE_SOURCE"] == "merge_request_event" else
                                "for an unprotected ref", kind["CI_COMMIT_REF_PROTECTED"], kind["CI_PIPELINE_SOURCE"]))
    for variable, spec in sorted(RUNNER_CLASSES.items()):
        for job in spec["jobs"]:
            if job not in classed.get(variable, []):
                problems.append("%s is one of %s's jobs (RUNNER_CLASSES), and %s" % (
                    job, variable, "no CI file defines it" if job not in definitions else
                    "it does not take its tags from that switch alone: tags: [$%s]" % variable))
    templates = sorted(set(re.findall(r"^\s*-\s*template:\s*(\S+)", "\n".join(every_ci.values()), re.MULTILINE)))
    notes.append("runners: %d job(s) take [$%s] (default: or their own) - %s for a protected ref that is not a merge "
                 "request's, %s for any other pipeline - and %d name a runner of the project's and run only for such a "
                 "ref, %d of them by a class's switch (%s), on linix unless the owner's project variable sends the class "
                 "to GitLab's runners; the jobs of GitLab's templates (%s) take default:'s" % (
                     len(chosen_runner), RUNNER_VARIABLE, DEFAULT_RUNNER, HOSTED_RUNNER, len(own),
                     sum(len(v) for v in classed.values()),
                     ", ".join("%s %d" % (v, len(classed.get(v, []))) for v in sorted(RUNNER_CLASSES)), ", ".join(templates)))
    return problems, notes


def runner_class_problems(texts, script=None):
    """[problem] for each way a class of jobs a switch moves to GitLab's hosted runners
    (RUNNER_CLASSES) could take a credential there, fail there, or run where it must not:
    a classed job that names an environment (a credential scoped to it would go with
    it), id_tokens or secrets; .gitlab-ci.yml's default: giving every job id_tokens, or
    taking keys by a merge key or an alias, which this does not read (no other CI file
    may set default:, ci_override_problems); a classed job that asks for a pull policy
    without always, which GitLab's runners refuse; one of an amd64 class in an arm64
    container, which they cannot run, or of the arm64 class in the CI image, which is
    amd64 alone; one that waits in a resource group other than its class's lane, or in
    any for a class without one; a job outside a class in a class's lane; a job that
    boots a QEMU machine without the tags that put it on server2 (KVM_JOBS); the checks
    of HOSTED_CHECKS_FILE missing, not included, not run as HOSTED_SCRIPT runs them, not
    on linix, not asking of every switch and lane whether it is at its default, or
    compute-quota not allowed to fail; the script's classes not these; and the GitLab
    templates whose variables the script knows (TEMPLATE_GLOBALS) not the ones the CI
    files include. script stands in for HOSTED_SCRIPT, for the self-test."""
    problems = []
    every_ci = ci_files(texts)
    order = [p for p in sorted(every_ci) if p != ".gitlab-ci.yml"] + [".gitlab-ci.yml"]
    jobs = {}
    for path in order:
        for name, body in blocks(every_ci[path] or "").items():
            jobs[name] = body
    lanes = dict((spec["lane"][0], variable) for variable, spec in RUNNER_CLASSES.items() if spec["lane"])
    for variable, spec in sorted(RUNNER_CLASSES.items()):
        for name in spec["jobs"]:
            if name not in jobs:
                continue
            taken = template_order(name, jobs)
            parts = [jobs[n] for n in taken]
            for part in list(parts):
                for other in re.findall(r"!reference \[\s*([^,\]\s]+)\s*,\s*(?:image|services)\s*\]", part):
                    parts += [jobs[n] for n in template_order(other, jobs)]
            joined = "\n".join(parts)
            for key in ("environment", "id_tokens", "secrets"):
                holder = next((n for n in taken if re.search(r"^  %s:" % key, jobs[n], re.MULTILINE)), None)
                if holder:
                    problems.append("%s, one of %s's jobs, says %s: (%s): %s, and %s's switch can send it to GitLab's runners - "
                                    "a job that needs it stays on the project's, out of the class" % (
                                        name, variable, key, holder, "a credential scoped to an environment goes to every job "
                                        "that names it" if key == "environment" else "it would be handed a credential or a "
                                        "token that signs as the pipeline", variable))
            for policy in re.findall(r"pull_policy:[ \t]*([^\n]*)", joined):
                value = re.split(r"\s#", policy, maxsplit=1)[0].strip()
                items = [v.strip() for v in value.strip("[]").split(",")] if value.startswith("[") else [value]
                if "always" not in items:
                    problems.append("%s, one of %s's jobs, asks for the pull policy %s: GitLab's hosted runners allow always "
                                    "alone and fail a job that asks for none of it; [if-not-present, always] keeps "
                                    "if-not-present on the project's runners" % (name, variable, value))
                    break
            in_ci_image = ".linix" in taken or re.search(r"!reference \[\s*\.linix\s*,\s*image\s*\]", joined)
            if spec["arch"] == "arm64" and in_ci_image:
                problems.append("%s, one of %s's jobs, runs in the CI image, which is amd64 alone: on GitLab's arm64 machines "
                                "it would not start" % (name, variable))
            if spec["arch"] != "arm64" and re.search(r"platform:[ \t]*linux/arm64", joined):
                problems.append("%s, one of %s's jobs, runs an arm64 container: GitLab's amd64 machines have no aarch64 "
                                "emulation, and its class is HM_RUNNER_ARM64" % (name, variable))
            group = None
            for n in taken:
                found = re.search(r"^  resource_group:[ \t]*(.*?)[ \t]*$", jobs[n], re.MULTILINE)
                if found:
                    group = yaml_scalar(found.group(1))
                    break
            lane = spec["lane"]
            if group is not None and (not lane or group != "$" + lane[0]):
                problems.append("%s, one of %s's jobs, waits in the resource group %s: %s" % (
                    name, variable, group, "a job of its class waits in the class's lane, $%s, which the owner gives each "
                    "job a lane of its own with on GitLab's runners" % lane[0] if lane else "its class has no lane, and "
                    "on GitLab's runners a group of one name holds back jobs that share no machine"))
    classed = set(job for spec in RUNNER_CLASSES.values() for job in spec["jobs"])
    for name in sorted(jobs):
        if name.startswith(".") or name in RESERVED or name in classed:
            continue
        for n in template_order(name, jobs):
            found = re.search(r"^  resource_group:[ \t]*(.*?)[ \t]*$", jobs[n], re.MULTILINE)
            if not found:
                continue
            lane = re.match(r"^\$\{?([A-Za-z0-9_]+)\}?$", yaml_scalar(found.group(1)))
            if lane and lane.group(1) in lanes:
                problems.append("%s waits in $%s, %s's lane, and is none of its jobs: on the project's runners it waits with "
                                "them, and with none once the owner gives each of them a lane of its own" % (
                                    name, lane.group(1), lanes[lane.group(1)]))
            break
    # default: gives each of its keys to every job that names none of its own - every job
    # of a class among them - so an ID token there would go with each to GitLab's runners.
    for line in (blocks(texts["ci"]).get("default") or "").split("\n"):
        code = re.split(r"(?:^|\s)#", line, maxsplit=1)[0]
        if re.search(r"""(?:^|[\s{,])["']?id_tokens["']?[ \t]*:""", code):
            problems.append(".gitlab-ci.yml's default: says id_tokens:, which GitLab gives every job that names none of its "
                            "own, every job of a class among them, and a switch sends those to GitLab's runners: a job that "
                            "needs a token names it itself, and stays on the project's runners")
            break
        if re.search(r"""(?:^|[\s{,])["']?<<["']?[ \t]*:|:[ \t]*\*[A-Za-z0-9_.\-]""", code):
            problems.append(".gitlab-ci.yml's default: takes keys by a merge key or an alias (%s), which this does not "
                            "read: whether it gives every job an ID token cannot be told" % code.strip()[:60])
            break
    for name, wanted in sorted(KVM_JOBS.items()):
        found = None
        try:
            for n in template_order(name, jobs):
                found = tag_values(jobs[n])
                if found is not None:
                    break
        except ValueError:
            found = None
        if found != wanted:
            problems.append("%s boots a QEMU machine, which wants KVM, and asks for the tags %s, not %s: on linix and s1-linux, "
                            "virtual machines without nested KVM, its NixOS test falls back to QEMU's TCG and timed out "
                            "(job 17129125235, 13 October 2026); server2 alone carries kvm" % (name, found, wanted))
        if not any(set(wanted) <= set(carried) for _, carried in PROJECT_RUNNERS):
            problems.append("no runner of the project's carries %s, which %s asks for (PROJECT_RUNNERS)" % (wanted, name))
    hosted = every_ci.get(HOSTED_CHECKS_FILE)
    if hosted is None:
        problems.append("%s is not in the tree: no pipeline a switch is set for would check the switches, the credentials "
                        "a job there is given or the namespace's compute minutes" % HOSTED_CHECKS_FILE)
    else:
        if not re.search(r"^  - local: %s\s*$" % re.escape(HOSTED_CHECKS_FILE), texts["ci"], re.MULTILINE):
            problems.append(".gitlab-ci.yml does not include %s: no pipeline a switch is set for would run its checks"
                            % HOSTED_CHECKS_FILE)
        found = blocks(hosted)
        refusal = found.get(".hosted-switched", "")
        for variable, default in sorted(SWITCH_DEFAULTS.items()):
            if '$%s == "%s"' % (variable, default) not in refusal:
                problems.append(".hosted-switched (%s) does not ask whether %s is %s: in a pipeline for which it alone is "
                                "set, neither check would run" % (HOSTED_CHECKS_FILE, variable, default))
        if not refusal.rstrip().endswith("- !reference [.own-runners, rules]"):
            problems.append(".hosted-switched (%s) does not end with .own-runners' rules: its checks would run where the "
                            "project's runners must take nothing, or for a push of documents alone" % HOSTED_CHECKS_FILE)
        for job, command in sorted(HOSTED_CHECKS.items()):
            body = found.get(job, "")
            if not body:
                problems.append("%s defines no job %s" % (HOSTED_CHECKS_FILE, job))
                continue
            if not re.search(r"^  tags: \[linix\]\s*$", body, re.MULTILINE):
                problems.append("%s (%s) does not run on linix alone: a check of the switches must not depend on them, and on "
                                "linix it runs when the minutes have run out" % (job, HOSTED_CHECKS_FILE))
            if "    - !reference [.hosted-switched, rules]" not in body:
                problems.append("%s (%s) does not take .hosted-switched's rules" % (job, HOSTED_CHECKS_FILE))
            if not re.search(r"^    - python3 build/ci/hosted-runners\.py %s\s*$" % command, body, re.MULTILINE):
                problems.append("%s (%s) does not run python3 build/ci/hosted-runners.py %s" % (job, HOSTED_CHECKS_FILE, command))
            environment = re.search(r"^  environment:\s*\n    name: (\S+)", body, re.MULTILINE)
            if job == "compute-quota" and (not environment or environment.group(1) != HOSTED_QUOTA_ENVIRONMENT):
                problems.append("compute-quota (%s) does not name the environment %s, the one HM_QUOTA_TOKEN is scoped to: it "
                                "would read no usage, or the token would be given to every job" % (
                                    HOSTED_CHECKS_FILE, HOSTED_QUOTA_ENVIRONMENT))
            if job != "compute-quota" and re.search(r"^  environment:", body, re.MULTILINE):
                problems.append("%s (%s) names an environment: it must be given what a job that names none is given, as every "
                                "job a switch moves is" % (job, HOSTED_CHECKS_FILE))
            if job == "compute-quota" and not re.search(r"^  allow_failure: true[ \t]*$", body, re.MULTILINE):
                problems.append("compute-quota (%s) does not say allow_failure: true: what it says of the minutes is for the "
                                "owner to read, and a failure of it would save no minute - no job waits for it - and hold a "
                                "gate whose every job had passed until a pipeline was made again" % HOSTED_CHECKS_FILE)
    try:
        module = script if script is not None else _load(HOSTED_SCRIPT, "hosted_runners")
        theirs = dict((c[0], (c[1], (c[2], c[3]) if c[2] else None)) for c in module.CLASSES)
        ours = dict((variable, (spec["arch"], spec["lane"])) for variable, spec in RUNNER_CLASSES.items())
        if theirs != ours:
            problems.append("%s's CLASSES are not RUNNER_CLASSES' switches, architectures and lanes: %s, where the CI files "
                            "have %s" % (HOSTED_SCRIPT.replace(os.sep, "/"), sorted(theirs.items()), sorted(ours.items())))
        if dict(module.HOSTED) != dict(HOSTED_MACHINES):
            problems.append("%s's HOSTED machines are not HOSTED_MACHINES" % HOSTED_SCRIPT.replace(os.sep, "/"))
        # The variables of GitLab's templates, which GitLab gives every job as the CI
        # files' own: hosted-runners knows them by the templates the CI files include.
        templates = set()
        for text in every_ci.values():
            templates |= set(re.findall(r"""^\s*-\s*template\s*:\s*["']?([^"'\s#]+)""", blocks(text or "").get("include") or "",
                                        re.MULTILINE))
        known = set(getattr(module, "TEMPLATE_GLOBALS", None) or {})
        for template in sorted(templates - known):
            problems.append("the CI files include GitLab's template %s, and %s's TEMPLATE_GLOBALS does not name its "
                            "variables: GitLab gives every job them, and hosted-runners would take each for a credential "
                            "and fail every pipeline a switch is set for - name them there, as the variables: of CI lint's "
                            "merged_yaml list them" % (template, HOSTED_SCRIPT.replace(os.sep, "/")))
        for template in sorted(known - templates):
            problems.append("%s's TEMPLATE_GLOBALS names %s, which no CI file includes: hosted-runners would take a project "
                            "variable of one of its names for one that holds no secret" % (
                                HOSTED_SCRIPT.replace(os.sep, "/"), template))
    except Exception as error:
        problems.append("%s could not be read: %s" % (HOSTED_SCRIPT.replace(os.sep, "/"), error))
    return problems


# What each Python script of build/land-via-mr.py's GATE_FILES runs before it imports
# anything but os and sys: Python puts a script's own directory first on sys.path, and a
# file a change put there named as one of Python's modules (build/ci/json.py, build/re.py)
# would run as the gate's code - or as the landing's, on the person's machine - in a change
# that leaves every file of GATE_FILES alone.
PATH_GUARD = ("sys.path[:] = [p for p in sys.path if os.path.normcase(os.path.realpath(p or os.curdir)) !=\n"
              "               os.path.normcase(os.path.dirname(os.path.realpath(__file__)))]")


def path_guard_problems(sources):
    """[problem] for each Python script of sources ({path: text}) whose top level does not
    run PATH_GUARD before it imports anything but os and sys."""
    problems = []
    for path, text in sorted(sources.items()):
        text = (text or "").replace("\r\n", "\n")
        try:
            tree = ast.parse(text)
        except SyntaxError as error:
            problems.append("%s cannot be read: %s" % (path, error))
            continue
        guarded = False
        for number, node in enumerate(tree.body):
            if number == 0 and isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant) and \
                    isinstance(node.value.value, str):
                continue
            if isinstance(node, ast.Import) and all(a.name in ("os", "sys") and a.asname is None for a in node.names):
                continue
            guarded = isinstance(node, ast.Assign) and ast.get_source_segment(text, node) == PATH_GUARD
            break
        if not guarded:
            problems.append("%s does not keep its own directory off sys.path before it imports anything but os and sys "
                            "(PATH_GUARD): a file a change put beside it, named as one of Python's modules, would run as "
                            "its code with no change to a file a landing asks a person's word for" % path)
    return problems


def gate_python_sources(landing):
    """{path: text} of each Python script of the landing's GATE_FILES, as the tree has it."""
    sources = {}
    for path in getattr(landing, "GATE_FILES", None) or ():
        if path.endswith(".py"):
            try:
                with open(os.path.join(ROOT, *path.split("/")), encoding="utf-8") as f:
                    sources[path] = f.read()
            except OSError:
                sources[path] = None
    return sources


_landing = None


def landing_module():
    """build/land-via-mr.py as a module, loaded once: its functions are what a landing runs."""
    global _landing
    if _landing is None:
        _landing = _load(FILES["land"], "land_via_mr")
    return _landing


# The readers of a Fulcio certificate's fields (1.3.6.1.4.1.57264.*), which no other file
# of build/ or .gitlab/ may hold as bytes: one that does reads a field by searching.
CERTIFICATE_READERS = ("build/ci/sigstore-certificate.py", "build/ci/sigstore-certificate.sh")


def certificate_module():
    return _load(os.path.join("build", "ci", "sigstore-certificate.py"), "sigstore_certificate")


def certificate_problems(module, shell=None, script=None, answer_of=None, environ=None):
    """(problems, notes): both certificate readers held to sigstore-certificate.py's cases."""
    return module.reader_problems(shell, script or module.SHELL_READER, environ, answer_of)


def scanned_texts():
    """{path: text} of each text file of build/ and .gitlab/, and .gitlab-ci.yml."""
    texts = {}
    for name in tree_files():
        if not (name.startswith(("build/", ".gitlab/")) or name == ".gitlab-ci.yml"):
            continue
        try:
            with open(os.path.join(ROOT, name), "rb") as f:
                texts[name] = f.read().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
    return texts


def base64_runs(data):
    """The base64 characters that data alone decides, at each of the three places a
    run of bytes can start in base64: what a search of a certificate's base64 for data
    would look for."""
    runs = []
    for shift in range(3):
        text = base64.b64encode(bytes(shift) + data).decode("ascii")
        first = -(-8 * shift // 6)
        runs.append(text[first:(8 * (shift + len(data))) // 6])
    return runs


def byte_search_problems(texts):
    """Each file of texts but the readers that holds the arc's DER OID element as far
    as 57264 - the first ten bytes of the repository id's - as bytes: as hex, in
    whatever spelling (its text with 0x, \\x and every character that is not a hex
    digit taken out), as decimal numbers with anything but a digit between them, or as
    the base64 of a certificate holding it. (None is written here, which would be a
    file holding it.)"""
    arc = certificate_module().REPOSITORY_ID_OID[:10]
    prefix = arc.hex()
    decimal = re.compile(r"(?<![0-9])0*" + r"[^0-9]{1,8}0*".join(str(b) for b in arc) + r"(?![0-9])")
    runs = base64_runs(arc)
    problems = []
    for name, text in sorted(texts.items()):
        if name in CERTIFICATE_READERS:
            continue
        if prefix in re.sub(r"0x|\\x|[^0-9a-f]", "", text.lower()) or decimal.search(text) \
                or any(run in text for run in runs):
            problems.append("%s holds the Sigstore OID arc 1.3.6.1.4.1.57264 as bytes: a certificate's field is read from "
                            "its extensions, with build/ci/sigstore-certificate.py, never by searching its bytes, which "
                            "its public key can fill" % name)
    return problems


def artifacthub_problems(texts, jobs, image_jobs):
    """Artifact Hub's metadata for the image's repository: the job that pushes it, its
    script and the tree's file. jobs are .gitlab-ci.yml's and the image file's."""
    problems = []
    job = image_jobs.get(ARTIFACTHUB_JOB, "")
    if not job:
        return ["%s has no %s job: Artifact Hub's metadata would not reach the image's repository" % (FILES["image"], ARTIFACTHUB_JOB)]
    items = [re.sub(r"\s+", " ", item.strip()) for item in rule_items(job, jobs)]
    release = [re.sub(r"\s+", " ", item.strip()) for item in rule_items(jobs.get(".release-rules", ""), jobs)]
    if not release or items != release + [re.sub(r"\s+", " ", ARTIFACTHUB_MANUAL)]:
        problems.append("%s's rules are not .release-rules and then the protected default branch's, by hand and allowed to fail "
                        "(%s): %s" % (ARTIFACTHUB_JOB, re.sub(r"\s+", " ", ARTIFACTHUB_MANUAL), items))
    if TAG_ONLY not in rules_text(ARTIFACTHUB_JOB, jobs):
        problems.append("%s does not run for a protected v* tag (.release-rules)" % ARTIFACTHUB_JOB)
    if any("$HM_PUBLISH_IMAGE" in item for item in items):
        problems.append("%s reads $HM_PUBLISH_IMAGE in its rules; its script reads it as the job runs" % ARTIFACTHUB_JOB)
    if re.search(r"^  trigger:", job, re.MULTILINE):
        problems.append("%s is a trigger" % ARTIFACTHUB_JOB)
    entries = need_entries(job)
    entry = entries.get("container-image", "")
    if sorted(entries) != ["container-image"] or not re.search(r"^\s*optional: true\s*$", entry, re.MULTILINE) or \
            not re.search(r"^\s*artifacts: false\s*$", entry, re.MULTILINE):
        problems.append("%s does not need container-image alone, optionally (the default branch's pipeline has none) and "
                        "without its artifacts" % ARTIFACTHUB_JOB)
    for name, block in sorted(jobs.items()):
        if ARTIFACTHUB_JOB in needs(block):
            problems.append("%s needs %s, which may fail and pushes nothing of the image: nothing may wait for it"
                            % (name, ARTIFACTHUB_JOB))
    own = re.search(r"^    name: (\S+)\s*$", job, re.MULTILINE)
    push = re.search(r"^    name: (\S+)\s*$", image_jobs.get("container-image", ""), re.MULTILINE)
    if not own or not push or own.group(1) != push.group(1) or not REGCTL_ALPINE.match(own.group(1)):
        problems.append("%s does not run in container-image's regctl image, pinned by digest (%s)"
                        % (ARTIFACTHUB_JOB, own.group(1) if own else "no image"))
    if "- sh build/ci/container-image-artifacthub.sh" not in job or not re.search(r"^      - container-image-artifacthub/\s*$", job, re.MULTILINE):
        problems.append("%s does not run build/ci/container-image-artifacthub.sh and keep container-image-artifacthub/" % ARTIFACTHUB_JOB)
    script = texts["image-artifacthub"]
    for needle, why in (("metadata=hmailserver/source/Server/platform/packaging/docker/artifacthub-repo.yml", "the tree's file"),
                        ("metadata_tag=artifacthub.io", "the tag Artifact Hub reads"),
                        ("config_type=application/vnd.cncf.artifacthub.config.v1+yaml", "the configuration's media type"),
                        ("layer_type=application/vnd.cncf.artifacthub.repository-metadata.layer.v1.yaml", "the layer Artifact Hub reads"),
                        ('regctl artifact put --config-type "$config_type" --config-file "$work/empty"', "the empty configuration"),
                        ('--file-media-type "$layer_type" --file artifacthub-repo.yml --file-title', "the file as the one layer, titled"),
                        ('regctl artifact get "$repository:$metadata_tag" --file-media-type "$layer_type"', "the layer read back"),
                        ('cmp -s "$out/artifacthub-repo.yml" "$metadata"', "and held to the file byte for byte"),
                        ('regctl registry login "$CI_REGISTRY" --user "$CI_REGISTRY_USER" --pass-stdin', "the password on standard input"),
                        ('if [ "$ids" = 0 ]; then', "nothing pushed without an ID")):
        if needle not in script:
            problems.append("%s no longer has %s (%s)" % (FILES["image-artifacthub"], needle, why))
    # What regctl is run to do: log in and out, read, and one artifact put. Any other
    # command of it - image copy, tag delete, manifest put, index create - could write
    # or remove a tag, whatever the tag is spelled as.
    puts = 0
    for number, line in enumerate(script.split("\n"), 1):
        if line.lstrip().startswith("#"):
            continue
        for tag in re.findall(r'"?\$\{?repository\}?:(\$\{?[A-Za-z_]+\}?|[A-Za-z0-9_.\-]*)', line):
            if tag != "$metadata_tag":
                problems.append("%s line %d names a tag other than artifacthub.io: %s" % (FILES["image-artifacthub"], number, line.strip()))
        for command, sub in re.findall(r"(?:^|[|&;(])\s*(?:\$\(\s*)?regctl\s+([A-Za-z_-]+)(?:\s+([A-Za-z_-]+))?", line):
            if command == "artifact" and sub == "put":
                puts += 1
            elif command != "version" and (command, sub) not in ARTIFACTHUB_REGCTL:
                problems.append("%s line %d runs regctl %s %s, which is not one of the commands it may run (%s): %s"
                                % (FILES["image-artifacthub"], number, command, sub,
                                   ", ".join("%s %s" % pair for pair in ARTIFACTHUB_REGCTL), line.strip()))
        if re.search(r"\$\{?CI_REGISTRY_PASSWORD", line) and "| regctl registry login" not in line and \
                not re.search(r'\[ -n "\$\{CI_REGISTRY_PASSWORD:-\}" \]', line):
            problems.append("%s line %d hands the registry password to something but regctl's standard input: %s"
                            % (FILES["image-artifacthub"], number, line.strip()))
    if puts != 1:
        problems.append("%s runs regctl artifact put %d times, not once" % (FILES["image-artifacthub"], puts))
    lines = texts["image-artifacthub-file"].split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    bad = [line for line in lines if not ARTIFACTHUB_LINE.match(line)]
    if not texts["image-artifacthub-file"] or bad:
        problems.append("%s holds a line that is not a comment, a blank line or \"repositoryID: <a UUID>\", which "
                        "container-image-artifacthub.sh would refuse to push: %s" % (FILES["image-artifacthub-file"], bad[:1]))
    if sum(1 for line in lines if line.startswith("repositoryID:")) > 1:
        problems.append("%s names more than one repository ID" % FILES["image-artifacthub-file"])
    return problems


def image_platform_problems(texts, jobs, image_jobs):
    """Every place a platform of the container image (IMAGE_PLATFORMS) could be left
    out: the Dockerfile, the build script, the branch's jobs, the tag's build, push,
    SBOM, scanning and start jobs, and the scripts they run. jobs are .gitlab-ci.yml's."""
    problems = []
    platforms = " ".join(IMAGE_PLATFORMS)
    dockerfile = texts["dockerfile"]
    for needle, why in (("\nARG TARGETARCH\n", "the architecture the build is for"),
                        ("\nARG HM_DEB=packages/hmailserver_*_${TARGETARCH}.deb\n", "the .deb of that architecture"),
                        ("\nCOPY ${HM_DEB} /tmp/hmailserver.deb\n", "that .deb copied in"),
                        ("\n && hmailserver --version\n", "the server run on the image's platform in the build")):
        if needle not in dockerfile:
            problems.append("%s no longer has %r (%s): an image could be built from the .deb of another architecture"
                            % (FILES["dockerfile"], needle.strip(), why))
    if re.search(r"^[^#\n]*hmailserver_\*?_?(amd64|arm64)\.deb", dockerfile, re.MULTILINE):
        problems.append("%s names one architecture's .deb: each platform's image takes its own" % FILES["dockerfile"])
    build = texts["image-build"]
    for needle, why in (('--custom-platform "linux/$arch"', "kaniko builds the platform asked for"),
                        ('--build-arg "TARGETARCH=$arch"', "the Dockerfile takes that architecture's .deb"),
                        ("\n  amd64|arm64) ;;\n", "the two platforms, and no other"),
                        ('grep -q "\\"architecture\\":\\"$arch\\"" "$out/image.json"', "the image is for that architecture"),
                        ('"hMailServer $expected build "', "the server answered in the build")):
        if needle not in build:
            problems.append("%s no longer has %s (%s)" % (FILES["image-build"], needle.strip(), why))

    # A branch's pipeline builds both platforms, and release waits for both.
    arm = image_jobs.get("linux-image-arm64", "")
    if not arm:
        problems.append("%s has no linux-image-arm64 job: a branch's pipeline would not build the linux/arm64 image" % FILES["image"])
    else:
        if not re.search(r"^    HM_IMAGE_ARCH: arm64\s*$", arm, re.MULTILINE) or "- sh build/ci/linux-image.sh" not in arm \
                or "- linux-image-arm64/" not in arm:
            problems.append("linux-image-arm64 does not build the linux/arm64 image with build/ci/linux-image.sh "
                            "(HM_IMAGE_ARCH: arm64) and keep linux-image-arm64/")
        if "artifacts: true" not in need_entries(arm).get("deb-ubuntu26.04-arm64", ""):
            problems.append("linux-image-arm64 does not need deb-ubuntu26.04-arm64 with its artifacts, whose .deb the release carries")
        if not re.search(r"^  extends: \.own-runners\s*$", arm, re.MULTILINE) or re.search(r"^  (allow_failure|when):", arm, re.MULTILINE):
            problems.append("linux-image-arm64 does not take .own-runners' rules alone, as linux-image does")
    release_needs = need_entries(jobs.get("release", ""))
    for name in ("linux-image", "linux-image-arm64"):
        if name not in release_needs:
            problems.append("release does not need %s: a release could be made whose image does not build for every platform" % name)

    # A tag's pipeline: each platform's image in a job of its own.
    for arch in IMAGE_PLATFORMS:
        name = "container-image-" + arch
        block = image_jobs.get(name, "")
        if not block:
            continue
        if not (re.search(r'^    HM_IMAGE_RELEASE: "1"\s*$', block, re.MULTILINE)
                and re.search(r"^    HM_IMAGE_ARCH: %s\s*$" % arch, block, re.MULTILINE)
                and "- sh build/ci/linux-image.sh" in block and ("- container-image-%s/" % arch) in block):
            problems.append("%s does not build the release's linux/%s image with build/ci/linux-image.sh (HM_IMAGE_RELEASE \"1\", "
                            "HM_IMAGE_ARCH: %s) and keep container-image-%s/ for container-image" % (name, arch, arch, arch))
        if "artifacts: true" not in need_entries(block).get("container-image-deb", ""):
            problems.append("%s does not take container-image-deb's artifact, the release's .deb files" % name)
    # And no job for a third platform: neither an image built, nor the twin of the scan or
    # of the start that container_scanning_arm64 and container-image-start-arm64 are.
    for name in image_jobs:
        for prefix, does, own in (("container-image-start-", "starts", ()),
                                  ("container_scanning_", "scans", ()),
                                  ("container-image-", "builds", ("deb", "sbom", "sign", "start", "artifacthub"))):
            if name.startswith(prefix):
                rest = name[len(prefix):]
                if rest not in own and rest not in IMAGE_PLATFORMS:
                    problems.append("%s %s an image of a platform the image is not built for (%s)" % (name, does, platforms))
                break

    # The chart tells Artifact Hub the image's platforms: exactly these, under the one
    # image it names, and helm-chart-package.sh holds the package to the same.
    block = re.search(r"^  artifacthub\.io/images: \|\n((?:    [^\n]*\n|[ \t]*\n)*)", texts["chart"] + "\n", re.MULTILINE)
    listed = [line.strip() for line in block.group(1).split("\n") if line.strip()] if block else []
    wanted = ["- name: hmailserver", "image: registry.gitlab.com/hmailserver/hmailserver:latest", "platforms:"] + \
        ["- linux/" + arch for arch in IMAGE_PLATFORMS]
    if listed != wanted:
        problems.append("%s's artifacthub.io/images is not the one image on exactly %s: %s"
                        % (FILES["chart"], ", ".join("linux/" + arch for arch in IMAGE_PLATFORMS), listed))
    if ('platforms="%s"' % " ".join("linux/" + arch for arch in IMAGE_PLATFORMS)) not in texts["chart-package"] or \
            texts["chart-package"].count('[ "$(images_block "$') != 2:
        problems.append("%s does not hold the tree's chart and the packaged one to the image's platforms, %s"
                        % (FILES["chart-package"], " ".join("linux/" + arch for arch in IMAGE_PLATFORMS)))

    # The push: both platforms' images, in regctl's image pinned by digest, under one index.
    push = image_jobs.get("container-image", "")
    if push:
        if "- sh build/ci/container-image-push.sh" not in push:
            problems.append("container-image does not run build/ci/container-image-push.sh")
        image = re.search(r"^    name: (\S+)\s*$", push, re.MULTILINE)
        pinned = REGCTL_ALPINE.match(image.group(1)) if image else None
        channels = CHANNELS_REGCTL.search(texts["ci-includes"].get(".gitlab/ci/channels.yml", ""))
        if not pinned:
            problems.append("container-image does not run in docker.io/regclient/regctl:<version>-alpine pinned by digest (%s)"
                            % (image.group(1) if image else "no image"))
        elif not channels or channels.group(1) != pinned.group(1):
            problems.append("container-image's regctl is %s, not the release HM_REGCTL_IMAGE pins in .gitlab/ci/channels.yml (%s)"
                            % (pinned.group(1), channels.group(1) if channels else "none"))
        for arch in IMAGE_PLATFORMS:
            if "artifacts: true" not in need_entries(push).get("container-image-" + arch, ""):
                problems.append("container-image does not take container-image-%s's artifact, the linux/%s image" % (arch, arch))
        if "dotenv: container-image/image.env" not in push:
            problems.append("container-image does not hand its dotenv report, the images by digest, to the jobs after it")
    script = texts["image-push"]
    for needle, why in (('platforms="%s"' % platforms, "both platforms' images"),
                        ('( cd "$dir" && sha256sum -c SHA256SUMS.txt )', "each tarball the one its job checked"),
                        ('grep -q "\\"architecture\\":\\"$arch\\"" "$dir/image.json" || fail',
                         "each image refused unless it is for the architecture its job is named for"),
                        ("--media-type application/vnd.docker.distribution.manifest.list.v2+json", "one index"),
                        ('--ref "$layout:amd64" --ref "$layout:arm64"', "naming both platforms' images"),
                        ('regctl image digest --platform "linux/$arch" "$layout:$version"', "each platform's entry checked"),
                        ('regctl image digest --platform "linux/$arch" "$repository@$index"', "and read back from the registry"),
                        ('echo "HM_IMAGE_AMD64=$repository@$digest_amd64"', "the linux/amd64 image named for the jobs after"),
                        ('echo "HM_IMAGE_ARM64=$repository@$digest_arm64"', "the linux/arm64 image named for the jobs after")):
        if needle not in script:
            problems.append("%s no longer has %s (%s)" % (FILES["image-push"], needle, why))
    checked = script.find("regctl index create")
    login = script.find("regctl registry login")
    if checked < 0 or login < 0 or login < checked:
        problems.append("%s logs in to the registry before both platforms' images and the index are made and checked" % FILES["image-push"])

    # Each platform's SBOM, scan and start.
    sbom = texts["image-sbom"]
    for needle, why in (("platforms=(%s)" % platforms, "every platform's SBOM"),
                        ('variable="HM_IMAGE_${arch^^}"', "each platform's image by its own digest"),
                        ('--image "$image" --arch "$arch"', "each document naming its platform"),
                        ('deb="$signed/hmailserver_${version}_${arch}.deb"', "from that architecture's .deb's own SBOM")):
        if needle not in sbom:
            problems.append("%s no longer has %s (%s)" % (FILES["image-sbom"], needle, why))
    scan = image_jobs.get("container_scanning_arm64", "")
    if scan and not (re.search(r"^  extends: container_scanning\s*$", scan, re.MULTILINE)
                     and re.search(r"^  dependencies: \[container-image\]\s*$", scan, re.MULTILINE)
                     and re.search(r"^    HM_SCAN_ARCH: arm64\s*$", scan, re.MULTILINE)):
        problems.append("container_scanning_arm64 does not scan the linux/arm64 image: it must extend container_scanning, "
                        "take container-image's dotenv report (dependencies: [container-image]) and set HM_SCAN_ARCH "
                        "to arm64, by which build/ci/container-scan.sh takes HM_IMAGE_ARM64 from that report")
    # And each scan's need of container-image must bring that job's artifacts: GitLab
    # hands a job that uses needs the dotenv reports of the needs with artifacts: true
    # alone (Ci::BuildDependencies#from_needs), which its dependencies only narrow, so a
    # need with artifacts: false, the dependencies line kept, is accepted and leaves the
    # job no image to scan - container-scan.sh exits 2, and the job is allowed to fail.
    # The job is read as GitLab's YAML reader, Psych, reads it, which takes the last of
    # two keys and no, off or False for false: each key of the job must be one, its needs
    # and its dependencies among them, and its entry for container-image must have one
    # job key, naming it, and one artifacts key, saying true. An entry without the key,
    # which GitLab takes for true, is refused as well, as every need this checker holds
    # to its artifacts is: the key is written out. Each job's own entry is the one GitLab
    # takes, since IMAGE_JOBS holds both to needs of their own that name container-image,
    # which replace the needs that container_scanning_arm64 would otherwise take from
    # container_scanning by extends, and GitLab refuses a job that needs one job twice
    # (YamlProcessor's validate_needs_specification!), so that no other entry, in
    # whatever form, can name container-image; and no rule the job takes may bring needs,
    # since a rule that matches replaces the job's needs whole (rules:needs).
    #
    # This reads the CI files as this tree writes them - a lint of them against drift,
    # not a YAML parser - and refuses every other form of what it holds that GitLab
    # reads, which it would read past: a key of the job, of a template whose rules it
    # could take, or of its entry for container-image that is not bare (unread_keys:
    # quoted, a space before the colon, a merge key); in the rules it takes and each
    # block their !reference [X, rules] names, what unread_rules does not read (a
    # second rules: key, anything after rules: on its line, a list not indented four, a
    # rule in flow style or by a !reference to anything but X's rules, an X outside
    # .gitlab-ci.yml and container-image.yml, the files it reads); either job, or a
    # template it reaches, defined more than once in the CI files, since GitLab merges a
    # later include's keys over an earlier one's and takes the last of two definitions
    # in one file; and a line at a CI file's top level that is not a key, such as a
    # merge key there, whose keys top_level_keys does not see. GitLab's own templates
    # are not read, nor what GitLab refuses itself when the pipeline is made (a need
    # from another pipeline beside dependencies, an alias without its anchor).
    merged = dict(jobs)
    merged.update(image_jobs)
    scans = (("container_scanning", "amd64"), ("container_scanning_arm64", "arm64"))
    for name, arch in scans:
        scan = image_jobs.get(name, "")
        keys = re.findall(r"^  ([a-z_]+):(?:[ \t]|$)", scan, re.MULTILINE)
        for key in sorted(set(key for key in keys if keys.count(key) > 1)):
            problems.append("%s has %d %s: keys: GitLab's YAML reader takes the last alone, where this checker reads the "
                            "first or any, and each key of the job - its needs, dependencies and rules, by which it takes "
                            "container-image's dotenv report, among them - must be one" % (name, keys.count(key), key))
        order = template_order(name, merged)
        deciding = [current for current in order if re.search(r"^  rules:", merged[current], re.MULTILINE)]
        if scan and any(re.search(r"^[ \t]*needs:", item, re.MULTILINE)
                        for item in (rule_items(merged[deciding[0]], merged) if deciding else [])):
            problems.append("%s takes a rule with needs of its own: a rule that matches replaces the job's needs whole "
                            "(rules:needs), and the job takes container-image's dotenv report by the needs of its own that "
                            "this checker holds" % name)
        entry = need_entries(scan).get("container-image", "")
        named = len(re.findall(r"^(?:      )?job:", entry, re.MULTILINE))
        if scan and named > 1:
            problems.append("%s's need of container-image has %d job: keys: GitLab's YAML reader takes the last alone, where "
                            "this checker reads the first, so that the entry it holds to artifacts: true would be another "
                            "job's need, and container-image's could be one without its artifacts" % (name, named))
        said = re.findall(r"^[ \t]*artifacts:[ \t]*(.*?)(?:[ \t]+#.*)?[ \t]*$", entry, re.MULTILINE)
        if scan and said != ["true"]:
            problems.append("%s does not need container-image with artifacts: true, its entry's one artifacts key (%s): "
                            "GitLab hands a job that uses needs the dotenv reports of the needs with artifacts alone, "
                            "whatever its dependencies say, and container-image's report names the linux/%s image by its "
                            "digest (HM_IMAGE_%s), which build/ci/container-scan.sh scans"
                            % (name, "it says " + ", ".join("artifacts: " + value for value in said) if said
                               else "it says nothing of artifacts", arch, arch.upper()))
        unread = unread_keys(scan, 2)
        for current in order[1:order.index(deciding[0]) + 1 if deciding else len(order)]:
            unread += ["%s: %s" % (current, line) for line in unread_keys(merged[current], 2)]
        unread += ["its need of container-image: %s" % line for line in unread_keys(entry.partition("\n")[2], 6)]
        if deciding:
            unread += ["the rules it takes, in %s" % what for what in unread_rules(deciding[0], merged)]
        if scan and unread:
            problems.append("%s is written in a form this checker does not read (%s): it reads the job's keys, those of its "
                            "need of container-image and the rules it takes only as this tree writes them - each key bare "
                            "(unquoted, its colon straight after its name), the rules a list indented four, each rule bare "
                            "keys or !reference [X, rules] with X a block of .gitlab-ci.yml or container-image.yml - where "
                            "GitLab's YAML reader takes other forms too, a quoted key, a space before a colon, a merge key "
                            "and a rule in flow style among them, so that container-image's dotenv report could be lost "
                            "past this check" % (name, unread[0]))
    every_ci = ci_files(texts)
    for reached, paths in sorted(reached_definitions(every_ci, [name for name, _ in scans]).items()):
        count = sum(top_level_keys(every_ci[path]).count(reached) for path in paths)
        if count > 1:
            problems.append("%s is defined %s: GitLab merges a later include's keys over an earlier one's and "
                            ".gitlab-ci.yml's over all, and of two definitions in one file takes the last, where this "
                            "checker reads one, so that the needs, dependencies or rules of container_scanning or "
                            "container_scanning_arm64 could be other than it reads - each, and every template they reach, "
                            "is defined once" % (reached, "in " + " and ".join(paths) if len(paths) > 1 else
                                                 "%s in %s" % ({2: "twice"}.get(count, "%d times" % count), paths[0])))
    for path, text in sorted(every_ci.items()):
        odd = [line for line in (text or "").split("\n")
               if line[:1] not in ("", " ", "\t", "#") and not TOP_LEVEL_KEY.match(line)]
        if odd:
            problems.append("%s has a line at the top level that is not a key this checker reads (%s): GitLab's YAML reader "
                            "takes a merge key there, or another such form, for keys of the file - a definition of "
                            "container_scanning or container_scanning_arm64 among them - which this does not see"
                            % (path, odd[0][:80]))
    start = image_jobs.get("container-image-start-arm64", "")
    command = re.search(r"^START_COMMAND='([^'\n]+)'$", texts["image-start"], re.MULTILINE)
    if start and not (re.search(r"^  extends: container-image-start\s*$", start, re.MULTILINE)
                      and re.search(r"^    - name: \$HM_IMAGE_ARM64\s*\n      alias: mail\s*\n      docker:\s*\n"
                                    r"        platform: linux/arm64\s*$", start, re.MULTILINE)
                      and command and '      command: ["bash", "-c", "%s"]' % command.group(1) in start):
        problems.append("container-image-start-arm64 does not start the linux/arm64 image (HM_IMAGE_ARM64) as the service mail "
                        "of the platform linux/arm64, with the test script's START_COMMAND, as container-image-start's twin")
    return problems


# container-image-push.sh's version_order, run under sh against these cases: A, B, and
# what A is against B. Numbers, not text: 6.3.10 is newer than 6.3.9.
VERSION_ORDER_CASES = (("6.3.10", "6.3.9", "newer"), ("6.3.9", "6.3.10", "older"), ("6.3.6", "6.3.6", "same"),
                       ("6.4.0", "6.3.99", "newer"), ("7.0.0", "6.10.10", "newer"), ("10.0.0", "9.0.0", "newer"),
                       ("6.3.5", "6.3.6", "older"), ("0.0.1", "0.0.0", "newer"))
# What each version_order text answered, so that the self-test's copies of the tree,
# which change other files, run sh once for it.
_version_order_answers = {}


def version_order_answers(function):
    """The answers of the shell function `function` (version_order) to each of
    VERSION_ORDER_CASES, from one run of sh; None where there is no sh."""
    if function in _version_order_answers:
        return _version_order_answers[function]
    shell = shutil.which("sh")
    if not shell:
        return None
    script = function + "\n" + "".join("version_order %s %s\n" % (a, b) for a, b, _ in VERSION_ORDER_CASES)
    try:
        run = subprocess.run([shell, "-c", script], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        answers = run.stdout.decode("utf-8", "replace").split()
    except (OSError, subprocess.SubprocessError) as error:
        answers = ["(sh: %s)" % error]
    _version_order_answers[function] = answers
    return answers


def image_tag_problems(texts):
    """(problems, notes): container-image-push.sh moves :latest and :<major>.<minor> only
    forward. It lists the tags that exist, makes a tag that does not, reads the version
    label of what an existing one names, moves it for this version or an older one,
    leaves it for a newer one and for one it cannot read, and puts no tag on the index
    but through move(); its version_order compares number by number, run here under sh."""
    problems, notes = [], []
    script = texts["image-push"]
    name = FILES["image-push"]
    plain = r"(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})"
    for needle, why in (("plain='%s'" % plain, "a version of plain numbers, compared as numbers"),
                        ('if printf \'%s\\n\' "$version" | grep -Eqx "$plain"; then\n  moving="${version%.*} latest"',
                         "only a version without a suffix moves them"),
                        ('regctl tag ls "$repository"', "the tags that exist, read from the registry"),
                        ("      keep \"$t\" \"the repository's tags could not be listed",
                         "each left where it is when the tags cannot be listed"),
                        ('    if ! grep -qxF "$t" "$work/tags"; then\n      move "$t" "it did not exist yet"',
                         "a tag made where there is none"),
                        ('current=$(regctl image config "$repository:$t" --platform linux/amd64 \\\n'
                         '      --format \'{{ index .Config.Labels "org.opencontainers.image.version" }}\'',
                         "the version the tag names now, read from its image's label"),
                        ('    if ! printf \'%s\\n\' "$current" | grep -Eqx "$plain"; then',
                         "a label that is not a plain version counted as unreadable"),
                        ('      keep "$t" "the image it names has no version this job can read',
                         "a tag whose version cannot be read left where it is"),
                        ('    case "$(version_order "$version" "$current")" in', "the two versions compared"),
                        ('      newer) move "$t"', "moved on for a newer version"),
                        ('      same) move "$t"', "moved again when this release is run again"),
                        ('      older) keep "$t"', "never moved back to an older version")):
        if needle not in script:
            problems.append("%s no longer has %s (%s): :latest or :<major>.<minor> could be moved backwards"
                            % (name, needle.splitlines()[0].strip(), why))
    # Every copy onto a tag of the repository: the version's own, and move()'s.
    copies = re.findall(r'regctl image copy "([^"]*)" "\$repository:([^"]*)"', script)
    mover = re.search(r"^move\(\) \{\n(.*?)^\}", script, re.MULTILINE | re.DOTALL)
    if sorted(copies) != sorted([("$layout:$version", "$version"), ("$repository@$index", "$1")]) or not mover \
            or 'regctl image copy "$repository@$index" "$repository:$1"' not in mover.group(1):
        problems.append("%s puts a tag on the index outside move(), which alone is called once the version the tag "
                        "names is compared: %s" % (name, ", ".join("%s -> :%s" % c for c in copies) or "no copy"))
    function = re.search(r"^version_order\(\) \{\n.*?^\}\n", script, re.MULTILINE | re.DOTALL)
    if not function:
        problems.append("%s has no version_order function: versions are not compared number by number" % name)
        return problems, notes
    answers = version_order_answers(function.group(0))
    if answers is None:
        if os.environ.get("CI") or os.environ.get("GITLAB_CI"):
            problems.append("there is no sh to run %s's version_order with" % name)
        else:
            notes.append("no sh here: %s's version_order was not run against its cases (CI runs it)" % name)
        return problems, notes
    wrong = ["%s against %s is %s, not %s" % (a, b, got, want) for (a, b, want), got
             in zip(VERSION_ORDER_CASES, answers + ["nothing"] * len(VERSION_ORDER_CASES)) if got != want]
    if wrong or len(answers) != len(VERSION_ORDER_CASES):
        problems.append("%s's version_order does not compare versions number by number: %s"
                        % (name, "; ".join(wrong) or "it answered %s" % " ".join(answers)))
    return problems, notes


def load_texts():
    """Each file of FILES by its key, and the tree's other texts. A file the tree lacks
    is read as empty and named in texts["missing"], which run_checks reports."""
    texts = {}
    missing = []
    for key, relative in FILES.items():
        try:
            texts[key] = read(relative)
        except (OSError, UnicodeDecodeError):
            texts[key] = ""
            missing.append(relative.replace(os.sep, "/"))
    texts["missing"] = missing
    texts["tree"] = tree_files()
    texts["permalinks"] = permalink_texts()
    texts["ci-includes"] = dict((name, read(name)) for name in texts["tree"]
                                if name.startswith(".gitlab/ci/") and name.endswith(".yml"))
    return texts


def self_test(chain, feed):
    texts = load_texts()
    failures = []
    problems, notes = check(texts, chain, feed)
    if problems:
        failures.append("the tree passes: %s" % problems)
    # cosign's and regctl's pins as the files have them: Renovate moves both
    # (renovate.json, the HM_*_IMAGE manager and the gitlabci manager), so the cases
    # below break what is there rather than a digest or a release written here. One
    # that is not found leaves its case's text missing, which fails the case.
    cosign = COSIGN_IMAGE.search(texts["ci"])
    cosign = cosign.group(1) if cosign else "HM_COSIGN_IMAGE not found"
    regctl = re.search(r"docker\.io/regclient/regctl:(v[0-9][A-Za-z0-9._\-]*)-alpine@sha256:", texts["image"])
    regctl = regctl.group(1) if regctl else "regctl not found"

    def broken(what, key, old, new, needle):
        copy = dict(texts)
        if old not in copy[key]:
            failures.append("%s: the text to break is not in %s (%r)" % (what, key, old))
            return
        copy[key] = copy[key].replace(old, new, 1)
        found, _ = check(copy, chain, feed)
        if not any(needle in p for p in found):
            failures.append("%s is caught: %s" % (what, found))

    broken("an ID token for another audience", "ci", "      aud: sigstore", "      aud: fulcio", "SIGSTORE_ID_TOKEN with aud sigstore")
    broken("Entra's audience changed", "ci", "aud: api://AzureADTokenExchange", "aud: https://gitlab.com", "AZURE_ID_TOKEN")
    held = chain.PINNED_IMAGE
    try:
        chain.PINNED_IMAGE = re.compile(r"^[a-z0-9.\-]+(?:/[a-z0-9._\-]+)+:[A-Za-z0-9._\-]+@sha256:[0-9a-f]{64}$")
        found, _ = check(texts, chain, feed)
        if not any("could put another registry's image" in p for p in found):
            failures.append("a run-time image check that takes any registry is caught: %s" % found)
    finally:
        chain.PINNED_IMAGE = held
    # The links' paths: 6.3.5's /downloads/<name>, a bundle kept apart from its file,
    # a check that would take the first, and a document naming a path no link has.
    for what, name, value, needle in (
            ("links under /downloads/, as 6.3.5's were", "link_path",
             lambda name, version: "/downloads/" + held_permalink(name, version), "/-/releases/<tag>/downloads/downloads/"),
            ("a bundle's path keeping the version", "permalink_name",
             lambda name, version: name if name.endswith(chain.BUNDLE) else held_permalink(name, version), "no longer both at permalink/latest"),
            ("a link check that takes a path under /downloads/", "link_problems", lambda links, description="": [],
             "link_problems takes a link whose path begins /downloads/")):
        held_permalink = chain.permalink_name
        held_value = getattr(chain, name)
        try:
            setattr(chain, name, value)
            found, _ = check(texts, chain, feed)
            if not any(needle in p for p in found):
                failures.append("%s is caught: %s" % (what, found))
        finally:
            setattr(chain, name, held_value)
    for what, planted in (("a versioned name", "hmailserver-9.9.9.tar.gz"), ("the old doubled path", "downloads/hMailServer-x64.exe"),
                          ("a name no release carries", "hMailServer.exe")):
        copy = dict(texts, permalinks=dict(texts["permalinks"], **{"hmailserver/docs/Planted.md":
                    "Download `https://gitlab.com/hmailserver/hmailserver/-/releases/permalink/latest/downloads/%s`.\n" % planted}))
        found, _ = check(copy, chain, feed)
        if not any("Planted.md gives the address" in p for p in found):
            failures.append("a document with %s at permalink/latest is caught: %s" % (what, found))
    if not texts["permalinks"] or not any(PERMALINK.search(t) for t in texts["permalinks"].values()):
        failures.append("the tree's permanent download addresses are read (README.md, Channels.md, the OBS _service): %s" % sorted(texts["permalinks"]))
    broken("cosign from ghcr.io", "ci", 'HM_COSIGN_IMAGE: "gcr.io/projectsigstore/cosign:', 'HM_COSIGN_IMAGE: "ghcr.io/sigstore/cosign:', "never ghcr.io")
    broken("the old bundle format", "sign", 'sign_flags="--yes --new-bundle-format', 'sign_flags="--yes', "--new-bundle-format")
    broken("another log", "sign", "--rekor-url https://rekor.sigstore.dev --fulcio", "--rekor-url https://log2025-1.rekor.sigstore.dev --fulcio", "Rekor v1 log")
    broken("the signing configuration left on", "sign", "--use-signing-config=false", "--use-signing-config=true", "signing configuration off")
    broken("another count of files", "sign", "expected=62", "expected=13", "a release carries")
    broken("verify counting another number of files", "verify", '[ "$verified" -eq 62 ]', '[ "$verified" -eq 13 ]', "release-verify.sh counts 13")
    # The AArch64 packages a release carries, from the package jobs.
    broken("release not waiting for the arm64 .deb's install", "ci", "    - job: deb-ubuntu26.04-arm64-install\n      artifacts: false\n", "",
           "release does not need deb-ubuntu26.04-arm64-install")
    broken("release not waiting for the aarch64 .rpm's build", "ci", "    - job: rpm-fedora44-aarch64\n      artifacts: true\n", "",
           "release does not need rpm-fedora44-aarch64")
    broken("release without the aarch64 .rpm's build artifacts", "ci", "    - job: rpm-fedora44-aarch64\n      artifacts: true\n",
           "    - job: rpm-fedora44-aarch64\n      artifacts: false\n", "release does not bring rpm-fedora44-aarch64's artifacts")
    broken("release fetching an install job's artifacts", "ci", "    - job: rpm-fedora44-aarch64-install\n      artifacts: false\n",
           "    - job: rpm-fedora44-aarch64-install\n      artifacts: true\n", "release brings rpm-fedora44-aarch64-install's artifacts")
    broken("release needing the arm64 .deb's SBOM optionally", "ci", "    - job: deb-ubuntu26.04-arm64-sbom\n      artifacts: true\n",
           "    - job: deb-ubuntu26.04-arm64-sbom\n      artifacts: true\n      optional: true\n", "release needs deb-ubuntu26.04-arm64-sbom optionally")
    broken("the arm64 .deb's SBOM job gone", "packages-deb", "\ndeb-ubuntu26.04-arm64-sbom:\n", "\ndeb-ubuntu26.04-arm64-sbom-old:\n",
           "has no job deb-ubuntu26.04-arm64-sbom")
    broken("the aarch64 .rpm's SBOM made before its build", "packages-rpm", "  needs:\n    - job: rpm-fedora44-aarch64\n      artifacts: true\n", "  needs: []\n",
           "rpm-fedora44-aarch64-sbom does not need rpm-fedora44-aarch64")
    broken("the arm64 .deb installed without its build", "packages-deb", "  needs: [deb-ubuntu26.04-arm64]\n", "  needs: []\n",
           "deb-ubuntu26.04-arm64-install does not need deb-ubuntu26.04-arm64")
    broken("the arm64 .deb's install test allowed to fail", "packages-deb", ".deb-install:\n  stage: test\n",
           ".deb-install:\n  stage: test\n  allow_failure: true\n",
           "deb-ubuntu26.04-arm64-install can let release start in a v* tag's pipeline without its having passed (.deb-install says allow_failure: true)")
    broken("the aarch64 .rpm's build left for a press on a tag", "packages-rpm",
           "    - if: $CI_COMMIT_TAG !~ /^v/ && $CI_COMMIT_BRANCH !~ /^batch/ && $CI_COMMIT_REF_PROTECTED == \"true\"\n      when: manual\n",
           "    - if: $CI_COMMIT_BRANCH !~ /^batch/ && $CI_COMMIT_REF_PROTECTED == \"true\"\n      when: manual\n",
           "rpm-fedora44-aarch64 can let release start in a v* tag's pipeline without its having passed (a rule a v* tag can match")
    broken("the arm64 .deb's SBOM job allowed to fail", "packages-deb", "deb-ubuntu26.04-arm64-sbom:\n  extends: .linix\n",
           "deb-ubuntu26.04-arm64-sbom:\n  extends: .linix\n  allow_failure: true\n",
           "deb-ubuntu26.04-arm64-sbom can let release start")
    broken("the arm64 .deb's SBOM kept elsewhere", "packages-deb", "      - packages-deb/ubuntu26.04-arm64/*.cdx.json\n", "      - packages-deb/*.cdx.json\n",
           "deb-ubuntu26.04-arm64-sbom does not run build/ci/packages/package-sbom.sh")
    broken("the installer signed before its SBOM is made", "ci", "    - job: release-sbom\n      artifacts: true\n", "",
           "release-sign does not need release-sbom")
    broken("the installer's SBOM made before Authenticode", "ci",
           "    - job: release-authenticode\n      artifacts: true\n  timeout: 10 minutes", "  timeout: 10 minutes",
           "release-sbom does not need release-authenticode")
    broken("the installer's SBOM left out of what is signed", "sign", 'cp "$sboms/$installer.cdx.json" "$out/$installer.cdx.json"\n', "",
           "the installer's SBOM among the files it signs")
    broken("the installer's SBOM not held to the signed bytes", "sign", 'grep -q "\\"content\\": \\"$signed_sha256\\""', 'grep -q "content"',
           "describes the signed installer")
    broken("the SBOMs not checked before publication", "verify", 'python3 build/asset-sbom.py verify "$file.cdx.json" "$file"',
           'true "$file.cdx.json" "$file"', "no longer holds each compiled file's SBOM")
    broken("publish not holding the SBOMs to their files", "chain", "    unfit = sbom_problems(assets, version)", "    unfit = []",
           "before it uploads")
    broken("the component list not kept for release-sbom", "ci", "sbom/native-dependencies-x64.json release-tools/", "release-tools/",
           "keep linux-sbom's component list")
    broken("a release started without a person", "ci", "      when: manual\n      allow_failure: false\n  before_script:",
           "      allow_failure: false\n  before_script:", "must wait for a person")
    broken("Sigstore before Authenticode", "ci", "    - job: release-authenticode\n      artifacts: true\n    - job: release-sbom\n      artifacts: true\n  timeout: 30 minutes\n  interruptible: false\n  rules:\n    - !reference [.release-rules, rules]\n  id_tokens:\n    SIGSTORE_ID_TOKEN",
           "    - job: release-sbom\n      artifacts: true\n  timeout: 30 minutes\n  interruptible: false\n  rules:\n    - !reference [.release-rules, rules]\n  id_tokens:\n    SIGSTORE_ID_TOKEN", "release-sign does not need release-authenticode")
    broken("a job that would run in a downstream pipeline", "ci", '.not-a-fuzz-pipeline:\n  rules:\n    - if: ' + REFUSAL,
           '.not-a-fuzz-pipeline:\n  rules:\n    - if: $CI_PIPELINE_SOURCE == "schedule"', "checkers would run in a downstream pipeline")
    # The feed after the release, in the tag's own pipeline.
    publish_need = "  needs:\n    - job: release-publish\n      artifacts: false\n      optional: true\n  timeout: 15 minutes"
    broken("pages at the start of a tag's pipeline, before the release exists", "ci", publish_need, "  needs: []\n  timeout: 15 minutes",
           "pages does not need release-publish")
    broken("pages needing release-publish where master's pipeline has none", "ci", publish_need,
           publish_need.replace("      optional: true\n", ""), "pages does not need release-publish, optionally")
    broken("pages bringing release-publish's artifacts", "ci", publish_need, publish_need.replace("artifacts: false", "artifacts: true"),
           "pages does not need release-publish, optionally and without its artifacts")
    broken("pages needing a gate too", "ci", publish_need, publish_need.replace("  timeout:", "    - job: checkers\n      artifacts: false\n  timeout:"),
           "and nothing else")
    broken("pages not for a v* tag", "ci", '    - if: $CI_COMMIT_TAG =~ /^v/ && $CI_COMMIT_REF_PROTECTED == "true"\n  script:\n    - python3 build/generate-update-feed.py --self-test',
           "  script:\n    - python3 build/generate-update-feed.py --self-test", "pages does not run for a protected v* tag")
    broken("package-repos without pages' site", "repos", "    - job: pages\n      artifacts: true\n", "",
           "package-repos does not need pages with its artifacts")
    broken("package-repos deploying nothing", "repos", "  pages: true\n", "  pages: false\n", "or does not deploy the site")
    trigger_job = ("\nupdate-feed:\n  stage: release\n  needs: [release-publish]\n  rules:\n    - !reference [.release-rules, rules]\n"
                   "  inherit:\n    variables: false\n  trigger:\n    project: Progressiverobot/hmailserver\n    branch: master\n")
    broken("6.3.5's update-feed trigger back", "ci", "\ninstaller-smoke:\n", trigger_job + "\ninstaller-smoke:\n",
           "update-feed in .gitlab-ci.yml triggers a pipeline of a project")
    broken("a trigger naming the group's path", "ci", "\ninstaller-smoke:\n", "\nfeed-again:\n  stage: release\n  trigger: hmailserver/hmailserver\n\ninstaller-smoke:\n",
           "feed-again in .gitlab-ci.yml triggers a pipeline of a project")
    included = sorted(texts["ci-includes"])[0]
    copy = dict(texts, **{"ci-includes": dict(texts["ci-includes"], **{included: texts["ci-includes"][included] + trigger_job})})
    found, _ = check(copy, chain, feed)
    if not any("update-feed in %s triggers a pipeline of a project" % included in p for p in found):
        failures.append("a project trigger in an included file (%s) is caught: %s" % (included, found))
    child = "\nchild:\n  stage: release\n  trigger:\n    include: .gitlab/ci/child.yml\n"
    copy = dict(texts, **{"ci-includes": dict(texts["ci-includes"], **{included: texts["ci-includes"][included] + child})})
    found, _ = check(copy, chain, feed)
    if any("triggers a pipeline of a project" in p for p in found):
        failures.append("a child pipeline's trigger (include) is not taken for a project's: %s" % found)
    broken("release started past the schema-upgrade gate", "ci", "    - job: linux-db-upgrade-mysql\n      artifacts: false\n", "",
           "release does not need linux-db-upgrade-mysql")
    broken("release fetching a gate's logs", "ci", "    - job: linux-suite\n      artifacts: false\n", "    - job: linux-suite\n      artifacts: true\n",
           "release brings linux-suite's artifacts")
    broken("a new gate release does not wait for", "ci", "\nlinux-cross:\n", "\nlinux-cross-2:\n  extends: .linix\n  script: [true]\n\nlinux-cross:\n",
           "release does not need linux-cross-2")
    broken("a release job for a branch", "ci", '.release-rules:\n  rules:\n    - if: $CI_PIPELINE_SOURCE == "schedule"',
           '.release-rules:\n  rules:\n    - if: $CI_PIPELINE_SOURCE == "scheduled"', ".release-rules no longer holds")
    # The schedules.
    broken("the fuzz job chosen by a variable again", "ci", '$CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "fuzz" &&',
           '$CI_PIPELINE_SOURCE == "schedule" && $HM_PIPELINE == "fuzz" &&', "linux-fuzz does not run for the schedule described \"fuzz\"")
    broken("the image pins not asked", "ci", "\nimage-pins:\n", "\nimage-pins-off:\n", "there is no image-pins job")
    broken("the image pins asked in every schedule's pipeline", "ci",
           '  rules:\n    - if: $CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "renovate" && '
           '$CI_COMMIT_REF_PROTECTED == "true"\n  retry:\n',
           '  rules:\n    - if: $CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION != "fuzz" && '
           '$CI_COMMIT_REF_PROTECTED == "true"\n  retry:\n',
           "image-pins does not run in the pipeline of the schedule described")
    broken("the image pins' check left out", "ci", "    - python3 build/check-image-pins.py\n\n",
           "    - python3 build/check-image-pins.py --list\n\n", "image-pins does not run build/check-image-pins.py")
    broken("a pin gone let pass", "ci", "image-pins:\n  extends: .no-github\n",
           "image-pins:\n  extends: .no-github\n  allow_failure: true\n", "image-pins says allow_failure")
    broken("the image pins retried on every failure", "ci", "    when: !reference [.gate-retry, retry, when]\n    exit_codes: [2]\n  script:\n"
           "    - python3 build/check-image-pins.py --self-test\n",
           "    exit_codes: [2]\n  script:\n    - python3 build/check-image-pins.py --self-test\n", "image-pins is not retried twice on exit 2")
    broken("Renovate chosen by a variable as well", "ci", '$CI_PIPELINE_SCHEDULE_DESCRIPTION == "renovate" && $CI_COMMIT_REF_PROTECTED',
           '$CI_PIPELINE_SCHEDULE_DESCRIPTION == "renovate" && $RUN_RENOVATE == "1" && $CI_COMMIT_REF_PROTECTED', "still reads $RUN_RENOVATE")
    broken("an old route back in a refusal", "security", '    - if: $CI_PIPELINE_SOURCE == "schedule" || $CI_PIPELINE_SOURCE == "pipeline"',
           '    - if: $HM_PIPELINE || $CI_PIPELINE_SOURCE == "schedule" || $CI_PIPELINE_SOURCE == "pipeline"', "still reads $HM_PIPELINE")
    for key in ("packages-deb", "packages-rpm", "packages-appimage"):
        broken("an old route back in %s's build refusal" % key, key,
               '    - if: $CI_PIPELINE_SOURCE == "schedule" || $CI_PIPELINE_SOURCE == "pipeline"',
               '    - if: $HM_PIPELINE || $CI_PIPELINE_SOURCE == "schedule" || $CI_PIPELINE_SOURCE == "pipeline"', "still reads $HM_PIPELINE")
    broken("pages in the coverage schedule's pipeline", "ci", ' || $CI_PIPELINE_SCHEDULE_DESCRIPTION == "coverage"', "",
           "pages would run in the pipeline of the schedule described \"coverage\"")
    broken("the coverage job without its schedule", "ci", '$CI_PIPELINE_SCHEDULE_DESCRIPTION == "coverage" &&',
           '$CI_PIPELINE_SCHEDULE_DESCRIPTION == "covered" &&', "linux-coverage does not run for the schedule described \"coverage\"")
    broken("pages in the fuzz schedule's pipeline", "ci", '($CI_PIPELINE_SCHEDULE_DESCRIPTION == "fuzz" || ', "(",
           "pages would run in the pipeline of the schedule described \"fuzz\"")
    broken("pages in the history scan's pipeline", "ci", ' || $CI_PIPELINE_SCHEDULE_DESCRIPTION == "security")', ")",
           "pages would run in the pipeline of the schedule described \"security\"")
    broken("pages in the forge export's pipeline", "ci", ' || $CI_PIPELINE_SCHEDULE_DESCRIPTION == "export"', "",
           "pages would run in the pipeline of the schedule described \"export\"")
    broken("the export chosen by a variable", "export", '$CI_PIPELINE_SCHEDULE_DESCRIPTION == "export"', '$HM_PIPELINE == "export"',
           "forge-export does not run for the schedule described \"export\"")
    broken("package-repos with rules of its own", "repos", "  rules:\n    - !reference [pages, rules]\n",
           "  rules:\n    - if: $CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH\n", "package-repos's rules are not pages' alone")
    broken("package-repos with a rule added", "repos", "  rules:\n    - !reference [pages, rules]\n",
           "  rules:\n    - if: $CI_PIPELINE_SOURCE == \"schedule\"\n    - !reference [pages, rules]\n", "package-repos's rules are not pages' alone")
    broken("the history scan without its schedule", "security", '$CI_PIPELINE_SCHEDULE_DESCRIPTION == "security"', '$CI_PIPELINE_SCHEDULE_DESCRIPTION == "secure"',
           "secret_detection does not run for the schedule described \"security\"")
    # Documentation-only pushes.
    broken("the compiled-in documents left out of the paths", "ci", '          - "[!.]*/**/*.md"\n', "",
           "paths match none of")
    broken("every Markdown file counted as code", "ci", '          - "[!.]*/**/*.md"\n', '          - "**/*.md"\n',
           "so a push changing only documentation would start the gates")
    broken("a job on a class's switch without the paths", "ci", "linux-sbom:\n  extends: .linix\n",
           "linux-sbom:\n  extends: .linix\n  rules:\n    - if: $CI_PIPELINE_SOURCE == \"schedule\" || $CI_PIPELINE_SOURCE == "
           "\"pipeline\"\n      when: never\n    - if: $CI_PIPELINE_SOURCE != \"merge_request_event\" && $CI_COMMIT_REF_PROTECTED == "
           "\"true\"\n", "linux-sbom runs on the project's own runners for a push that changes only documentation")
    broken("a job on linix without the paths", "ci", "      changes:\n        paths: *not-docs-only\n      when: manual", "      when: manual",
           "db-matrix runs on the project's own runners for a push that changes only documentation")
    broken("a pattern GitLab would read otherwise", "ci", '          - "**/*[!d]"', '          - "**/*{[!d],x}"', "braces are not read here")
    # The gates.
    broken("the Windows suite cancelled by a later push", "ci", "  timeout: 4 hours\n  # A later push does not cut the suite off half-way, nor leave the bench mid-build.\n  interruptible: false",
           "  timeout: 4 hours", "windows-gate can be cancelled half-way")
    broken("a busy bench let past in a tag's pipeline", "ci", "if ($taken -eq 1 -and -not $env:CI_COMMIT_TAG) { \"the bench is busy - no gate",
           "if ($taken -eq 1) { \"the bench is busy - no gate", "windows-gate lets an exit code fail")
    broken("the cosign placeholder back", "ci", '"%s"' % cosign, '"%s@sha256:PIN-THE-DIGEST"' % cosign.split("@")[0],
           "is the placeholder")
    # The fuzz job's findings, and the secret scan's.
    broken("fuzz findings public", "ci", "    access: developer\n    paths:\n      - fuzz-results/", "    paths:\n      - fuzz-results/",
           "linux-fuzz does not keep fuzz-results/")
    broken("a finding's report printed in the public log", "fuzz", "2> /dev/null | head -n 1)\n",
           "2> /dev/null | head -n 1)\n    tail -n 60 \"$report\"\n", "prints a finding's report into the job log")
    broken("a finding's place in the public log", "fuzz", "| sed -E 's/^(SUMMARY:", "| cat | sed -E 's/^(SUMMARX:",
           "prints a sanitizer's SUMMARY whole")
    broken("fuzz findings claimed by another job", "ci", "    paths:\n      - security/repo-hygiene/", "    paths:\n      - fuzz-results/\n      - security/repo-hygiene/",
           "repo-hygiene claims fuzz-results/")
    broken("a secret found as a warning", "security", "  allow_failure: false\n  rules:\n    - if: $SECRET_DETECTION_DISABLED",
           "  rules:\n    - if: $SECRET_DETECTION_DISABLED", "secret_detection does not say allow_failure: false")
    secrets = MIRRORED_SECRETS.search(texts["security"])
    secrets_pin = secrets.group(0) if secrets else "the mirrored secrets image is not found"
    broken("secret_detection from GitLab's registry again", "security", secrets_pin,
           "  image: registry.gitlab.com/security-products/secrets:7@sha256:%s" % (secrets.group(2) if secrets else ""),
           "secret_detection does not run GitLab's analyzer as the project's registry holds it")
    broken("secret_detection's image named over two lines, which build/scan-secrets.py does not read", "security",
           secrets_pin, secrets_pin.replace("  image: ", "  image:\n    name: ", 1),
           "secret_detection does not run GitLab's analyzer as the project's registry holds it")
    broken("the secrets mirror's tag naming another digest", "security", secrets_pin,
           secrets_pin.replace("-%s@" % (secrets.group(1) if secrets else ""), "-000000000000@"),
           "secret_detection's image tag ends in")
    broken("the analyzer's exit taken as the verdict", "security", "      if grep -Eq '\"vulnerabilities\" *: *\\[ *\\]' \"$report\"; then", "      if true; then",
           "secret_detection does not read its report")
    broken("the verifier trusting another file", "verifier", '//.gitlab-ci.yml@refs/tags/v"', '//.gitlab/ci.yml@refs/tags/v"', "GITLAB_IDENTITY_PREFIX")
    broken("the verifier trusting another group path", "verifier", 'GITLAB_GROUP_IDENTITY_PREFIX = "https://gitlab.com/hmailserver/hmailserver//',
           'GITLAB_GROUP_IDENTITY_PREFIX = "https://gitlab.com/hmailserver//', "GITLAB_GROUP_IDENTITY_PREFIX")
    broken("the group's signer with its id optional", "verifier", 'GITLAB_GROUP_REPOSITORY, GITLAB_REPOSITORY_ID, false, 0, ""',
           'GITLAB_GROUP_REPOSITORY, GITLAB_REPOSITORY_ID, true, 0, ""', "the project's id required")
    broken("the group's signer on another id", "verifier", 'GITLAB_GROUP_REPOSITORY, GITLAB_REPOSITORY_ID, false',
           'GITLAB_GROUP_REPOSITORY, GITHUB_REPOSITORY_ID, false', "the project's id required")
    broken("the first path's signer with its id optional again", "verifier", 'GITLAB_IDENTITY_PREFIX, GITLAB_ISSUER, GITLAB_REPOSITORY, GITLAB_REPOSITORY_ID, false, 0, ""',
           'GITLAB_IDENTITY_PREFIX, GITLAB_ISSUER, GITLAB_REPOSITORY, GITLAB_REPOSITORY_ID, true, 0, ""', "the path the project began at")
    broken("the first path's signer on another id", "verifier", 'GITLAB_IDENTITY_PREFIX, GITLAB_ISSUER, GITLAB_REPOSITORY, GITLAB_REPOSITORY_ID, false',
           'GITLAB_IDENTITY_PREFIX, GITLAB_ISSUER, GITLAB_REPOSITORY, GITHUB_REPOSITORY_ID, false', "the path the project began at")
    broken("the first path left to the settings", "verifier", "projectPaths[] = { GITLAB_REPOSITORY, GITLAB_GROUP_REPOSITORY };",
           "projectPaths[] = { GITLAB_GROUP_REPOSITORY };", "whatever the settings")
    broken("a script signing from a third path", "verify", "  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;\n",
           "  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;\n  someone/hmailserver) repository=https://gitlab.com/someone/hmailserver ;;\n",
           "release-verify.sh accepts the paths")
    broken("a script signing from the project's first path again", "sign",
           "  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;\n",
           "  Progressiverobot/hmailserver) repository=https://gitlab.com/Progressiverobot/hmailserver ;;\n"
           "  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;\n", "release-sign.sh accepts the paths")
    broken("the image signed from the project's first path again", "image-sign",
           "  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;\n",
           "  Progressiverobot/hmailserver) repository=https://gitlab.com/Progressiverobot/hmailserver ;;\n"
           "  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;\n",
           "container-image-sign.sh signs from another path than")
    broken("the verifier no longer trusting the path that signed 6.3.5", "verifier",
           'GITLAB_IDENTITY_PREFIX = "https://gitlab.com/Progressiverobot/hmailserver//',
           'GITLAB_IDENTITY_PREFIX = "https://gitlab.com/hmailserver/hmailserver//', "the path the project began at")
    broken("a script mapping a path to another repository", "sign", "  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;",
           "  hmailserver/hmailserver) repository=https://gitlab.com/Progressiverobot/hmailserver ;;", "release-sign.sh accepts the paths")
    broken("a script expecting a branch's identity", "sign", 'identity="${repository}//.gitlab-ci.yml@${ref}"',
           'identity="${repository}//.gitlab-ci.yml@refs/heads/${tag}"', "release-sign.sh expects another identity")
    broken("a release signed as a branch", "sign", TAG_REF, 'ref="refs/heads/${tag}"', "release-sign.sh names the ref it signs as")
    broken("a release's ref set before the tag is held to v*", "verify", "    v[0-9]*) ;;\n    *) fail \"this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')\" ;;\n  esac\n  version=\"${tag#v}\"\n",
           "    *) ;;\n  esac\n  version=\"${tag#v}\"\n", "release-verify.sh signs a release as")
    broken("a branch's identity without --rehearse", "sign", "  --rehearse) rehearse=1 ;;", "  --rehearse) ;;\n  *) rehearse=1 ;;",
           "release-sign.sh signs as a branch")
    broken("a rehearsal on any branch", "sign", "    batch*) ;;\n    *) fail \"a release is rehearsed", "    *) ;;\n    none) fail \"a release is rehearsed",
           "release-sign.sh signs as a branch")
    broken("a rehearsal on an unprotected branch", "sign", '[ "${CI_COMMIT_REF_PROTECTED:-}" = true ] || fail', 'true || fail',
           "release-sign.sh signs as a branch")
    broken("a rehearsal in a tag's pipeline", "verify", '[ -z "$tag" ] || fail', 'true || fail', "release-verify.sh signs as a branch")
    held_paths = chain.SIGNING_REPOSITORIES
    try:
        for what, path in (("a third path", "someone/hmailserver"), ("the project's first path", "Progressiverobot/hmailserver")):
            chain.SIGNING_REPOSITORIES = dict(held_paths, **{path: "https://gitlab.com/" + path})
            found, _ = check(texts, chain, feed)
            if not any("signs its provenance for" in p for p in found):
                failures.append("a provenance identity for %s is caught: %s" % (what, found))
    finally:
        chain.SIGNING_REPOSITORIES = held_paths
    broken("a third path, indented otherwise", "verify", "  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;\n",
           "  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;\n    someone/hmailserver) repository=https://gitlab.com/someone/hmailserver;;\n",
           "release-verify.sh accepts the paths")
    broken("any other path taken as it is", "sign", "  *) fail \"the project's path is '${CI_PROJECT_PATH:-}', not one a server trusts a signature from\" ;;\n",
           "  *) repository=\"https://gitlab.com/${CI_PROJECT_PATH}\" ;;\n", "release-sign.sh accepts the paths")
    broken("the repository named again after the case", "sign", SCRIPT_IDENTITY + "\n", SCRIPT_IDENTITY + "\nrepository=\"$CI_PROJECT_URL\"\n",
           "release-sign.sh accepts the paths")
    broken("the identity named again after the case", "verify", SCRIPT_IDENTITY + "\n",
           SCRIPT_IDENTITY + '\nidentity="${CI_PROJECT_URL}//.gitlab-ci.yml@refs/tags/${tag}"\n', "release-verify.sh expects another identity")
    group_line = 'trust.signers.push_back(MakeSigner_(GITLAB_GROUP_IDENTITY_PREFIX, GITLAB_ISSUER, GITLAB_GROUP_REPOSITORY, GITLAB_REPOSITORY_ID, false, 0, ""));'
    broken("a second group signer before it, its id optional", "verifier", group_line,
           group_line.replace(", false, 0,", ", true, 0,") + "\n      " + group_line, "the project's id required")
    broken("the project's id left to the settings", "verifier", "         else if (!projectRefusal.IsEmpty())\n            why = projectRefusal;\n", "",
           "whatever the settings")
    broken("a signature without a timestamp", "authenticode", "/tr $timestampUrl /td SHA256", "/td SHA256", "RFC 3161 timestamp")
    broken("the .deb taken before the release is published", "image", "    - job: release-publish\n      artifacts: false\n", "",
           "container-image-deb does not need release-publish")
    broken("the image built for a branch", "image", "    - !reference [.release-rules, rules]\n  variables:\n    HM_IMAGE_RELEASE",
           "    - if: $CI_COMMIT_BRANCH\n  variables:\n    HM_IMAGE_RELEASE", "container-image-amd64 can run for something that is not a protected v* tag")
    broken("the image pushed for a branch", "image", "    - !reference [.release-rules, rules]\n  script:\n    - sh build/ci/container-image-push.sh",
           "    - if: $CI_COMMIT_BRANCH\n  script:\n    - sh build/ci/container-image-push.sh", "container-image can run for something that is not a protected v* tag")
    broken("the image built before the .deb is taken", "image", "  needs:\n    - job: container-image-deb\n      artifacts: true\n  timeout: 30 minutes",
           "  needs: []\n  timeout: 30 minutes", "container-image-amd64 does not need container-image-deb")
    broken("the image started before it is pushed", "image",
           "  needs:\n    - job: container-image\n      artifacts: true\n  timeout: 20 minutes", "  needs: []\n  timeout: 20 minutes",
           "container-image-start does not need container-image")
    broken("the image pushed without a person", "image", "      when: manual\n      allow_failure: false\n  script:\n    - sh build/ci/container-image-deb.sh",
           "      allow_failure: false\n  script:\n    - sh build/ci/container-image-deb.sh", "container-image-deb is not started by hand")
    broken("the image built after the .deb was refused", "image", "      when: manual\n      allow_failure: false\n  script:\n    - sh build/ci/container-image-deb.sh",
           "      when: manual\n      allow_failure: true\n  script:\n    - sh build/ci/container-image-deb.sh", "container-image-deb may fail without stopping")
    broken("the switch decided when the pipeline is made", "image",
           "container-image-deb:\n  stage: release\n", 'container-image-deb:\n  stage: release\n  rules:\n    - if: $HM_PUBLISH_IMAGE != "1"\n      when: never\n',
           "container-image-deb reads $HM_PUBLISH_IMAGE in its rules")
    broken("the switch read by a rule", "image", "  rules:\n    - if: $CONTAINER_SCANNING_DISABLED",
           '  rules:\n    - if: $HM_PUBLISH_IMAGE != "1"\n      when: never\n    - if: $CONTAINER_SCANNING_DISABLED',
           "container_scanning reads $HM_PUBLISH_IMAGE in its rules")
    broken("the .deb taken without the switch", "image-deb", RUNTIME_GATE, 'if false; then', "container-image-deb.sh does not refuse")
    broken("the image pushed without the switch", "image-build", RUNTIME_GATE, 'if false; then', "linux-image.sh does not refuse")
    broken("the switch's refusal a success", "image-deb", "  exit 3\nfi\n\ntag=", "  exit 0\nfi\n\ntag=", "container-image-deb.sh does not refuse")
    broken("the refusal past its own fi", "image-build", "    exit 3\n  fi\n  # A release's image", "  fi\n  # A release's image",
           "linux-image.sh does not refuse")
    broken("the image pushed from the project's first path", "image-build", "    registry.gitlab.com/hmailserver/hmailserver) ;;\n",
           "    registry.gitlab.com/progressiverobot/hmailserver | registry.gitlab.com/hmailserver/hmailserver) ;;\n",
           "does not push to registry.gitlab.com/hmailserver/hmailserver alone")
    broken("the image built from release-sign's artifact", "image", "    HM_IMAGE_ARCH: amd64\n    HM_IMAGE_DEB_DIR: container-image-deb\n",
           "    HM_IMAGE_ARCH: amd64\n    HM_IMAGE_DEB_DIR: release-signed\n", "container-image-amd64 does not take the .deb from container-image-deb")
    broken("the .deb taken unverified", "image-deb", '--certificate-identity "$candidate" --certificate-oidc-issuer "$issuer"',
           '--certificate-identity-regexp ".*" --certificate-oidc-issuer-regexp ".*"', "as this project's pipeline")
    broken("a certificate without the project's id", "image-deb", '      absent) fail "the certificate of $name records no repository id" ;;',
           '      absent) id_said="none recorded" ;;', "the id required at either path")
    broken("a token sent where TLS is not checked", "image-deb", 'wget -q -U hmailserver-container-image',
           'wget -q --header "JOB-TOKEN: $CI_JOB_TOKEN" -U hmailserver-container-image', "sends or names a credential")
    broken("the image started otherwise in the job", "image", "--apply-new-install-defaults && exec", "--foreground || exec",
           "does not run build/ci/container-image-start-test.sh's START_COMMAND")
    broken("the start test without a network of its own", "image", '    FF_NETWORK_PER_BUILD: "true"\n', "",
           "container-image-start has no network of its own")
    broken("the start test on another image", "image", "    - name: $HM_IMAGE\n", "    - name: registry.gitlab.com/hmailserver/hmailserver:latest\n",
           "container-image-start does not start the pushed image by its digest")
    broken("the switch turned on in the job", "image", "  variables:\n    HM_IMAGE_RELEASE: \"1\"\n",
           "  variables:\n    HM_PUBLISH_IMAGE: \"1\"\n    HM_IMAGE_RELEASE: \"1\"\n", "sets HM_PUBLISH_IMAGE")
    broken("the switch turned on for every job", "ci", "\nvariables:\n", "\nvariables:\n  HM_PUBLISH_IMAGE: \"1\"\n", "sets HM_PUBLISH_IMAGE")
    broken("the image signed for another audience", "image", "      aud: sigstore", "      aud: fulcio", "container-image-sign has no id_tokens")
    broken("the image signed before it is pushed", "image", "  needs:\n    - job: container-image\n      artifacts: true\n    - job: container-image-sbom\n      artifacts: true\n"
           "    - job: container-image-start\n      artifacts: false\n    - job: container-image-start-arm64\n      artifacts: false\n  timeout: 15 minutes",
           "  needs: []\n  timeout: 15 minutes", "container-image-sign does not need container-image")
    broken("the scan without the image's name", "image", "  dependencies: [container-image]\n", "", "container_scanning does not take")
    # container_scanning's scanner: Trivy in dependency-scan's image, the pin as the file has
    # it (Renovate moves it), and never GitLab's analyzer or template again.
    scanner = CONTAINER_SCANNER.search(blocks(texts["image"]).get("container_scanning", ""))
    scanner_pin = scanner.group(0).rstrip() if scanner else "container_scanning's Trivy is not found"
    trivy_release = re.search(r"aquasec/trivy:([0-9.]+)@", scanner_pin)
    broken("container_scanning's Trivy by its tag alone", "image", scanner_pin, scanner_pin.split("@")[0],
           "container_scanning does not run in dependency-scan's Trivy image")
    broken("container_scanning's Trivy another release than dependency-scan's", "image", scanner_pin,
           scanner_pin.replace("aquasec/trivy:%s@" % (trivy_release.group(1) if trivy_release else "?"), "aquasec/trivy:0.1.0@"),
           "container_scanning does not run in dependency-scan's Trivy image")
    broken("container_scanning's Trivy named on the image: line", "image", "  image:\n" + scanner_pin + "\n    entrypoint: [\"\"]\n",
           scanner_pin.replace("    name: ", "  image: ") + "\n", "container_scanning does not run in")
    broken("GitLab's analyzer in container_scanning again", "image", "  image:\n" + scanner_pin + "\n    entrypoint: [\"\"]\n",
           "  image: registry.gitlab.com/security-products/container-scanning:8@sha256:%s\n" % ("1" * 64),
           "names GitLab's container-scanning analyzer")
    broken("GitLab's template included again", "image", "\ncontainer_scanning:\n",
           "\ninclude:\n  - template: Jobs/Container-Scanning.gitlab-ci.yml\n\ncontainer_scanning:\n",
           "names GitLab's container-scanning analyzer or includes its template")
    broken("GitLab's analyzer named in another CI file", "ci", "\nvariables:\n",
           "\nvariables:\n  CS_ANALYZER_IMAGE: registry.gitlab.com/security-products/container-scanning:8\n",
           ".gitlab-ci.yml names GitLab's container-scanning analyzer")
    broken("the scan without its script", "image", "    - sh build/ci/container-scan.sh\n", "    - trivy image \"$HM_IMAGE_AMD64\"\n",
           "container_scanning does not run build/ci/container-scan.sh")
    broken("the scan's report not kept", "image",
           "    reports:\n      container_scanning: container-scan/gl-container-scanning-report.json\n", "",
           "container_scanning does not keep the script's report")
    broken("the amd64 scan of the arm64 image", "image", "    HM_SCAN_ARCH: amd64\n", "    HM_SCAN_ARCH: arm64\n",
           "container_scanning does not scan the linux/amd64 image")
    broken("the arm64 scan in an image of its own", "image", "container_scanning_arm64:\n  extends: container_scanning\n",
           "container_scanning_arm64:\n  extends: container_scanning\n  image: registry.example/scanner:1\n",
           "container_scanning_arm64 has an image, a script or artifacts of its own")
    broken("the arm64 scan with a script of its own", "image", "    HM_SCAN_ARCH: arm64\n",
           "    HM_SCAN_ARCH: arm64\n  script:\n    - trivy image \"$HM_IMAGE_ARM64\"\n",
           "container_scanning_arm64 has an image, a script or artifacts of its own")
    broken("the scan script taking amd64's image for arm64", "image-scan", "  arm64) image=${HM_IMAGE_ARM64:-} ;;\n",
           "  arm64) image=${HM_IMAGE_AMD64:-} ;;\n", "no longer has arm64) image=${HM_IMAGE_ARM64:-} ;;")
    broken("the scan script taking any registry's image", "image-scan", 'digest=${image#"$REPOSITORY@sha256:"}\n',
           'digest=${image#*@sha256:}\n', "no longer has digest=${image#")
    broken("the scan script taking a database from GitHub", "image-scan",
           'DATABASES="mirror.gcr.io/aquasec/trivy-db:2 public.ecr.aws/aquasecurity/trivy-db:2"\n',
           'DATABASES="ghcr.io/aquasecurity/trivy-db:2"\n', "no longer has DATABASES=")
    broken("the scan script's report not in GitLab's format", "image-scan", "--template @/contrib/gitlab.tpl", "--template @/contrib/html.tpl",
           "no longer has trivy convert --format template")
    broken("the image signed before its SBOM is made", "image", "    - job: container-image-sbom\n      artifacts: true\n", "",
           "container-image-sign does not need container-image-sbom")
    broken("the image's SBOM made from release-sign's artifact", "image", "  variables:\n    HM_IMAGE_DEB_DIR: container-image-deb\n  script:\n    - bash",
           "  variables:\n    HM_IMAGE_DEB_DIR: release-signed\n  script:\n    - bash", "container-image-sbom does not take the .deb from container-image-deb")
    broken("the image signed without its SBOM attested", "image-sign", 'cosign attest $attest_flags --type cyclonedx --predicate "$sbom" "$image"',
           'true', "the image would go out without its SBOM")
    broken("the image's certificates kept no longer", "image-sign", ' --output-certificate "$out/certificate-$1.pem" "$2"', ' "$2"',
           "the certificate of each signature kept")
    broken("the image's signatures no longer read for the id", "image-sign", '\n  certificate_id "$1"\n}\n', "\n}\n",
           "each signature's certificate read for the project's id")
    broken("the image's signatures taken with any id", "image-sign", '\n    "id=$project_id") ;;\n', "\n    id=*) ;;\n",
           "only the project's id taken")
    broken("the image file not included", "ci", "  - local: .gitlab/ci/container-image.yml\n", "", "does not include .gitlab/ci/container-image.yml")
    broken("the image signed as a branch", "image-sign", TAG_SCRIPT_IDENTITY,
           'identity="${repository}//.gitlab-ci.yml@refs/heads/${tag}"', "container-image-sign.sh expects another identity")
    # Each platform of the image, left out somewhere (IMAGE_PLATFORMS).
    broken("the Dockerfile's .deb fixed to amd64", "dockerfile", "ARG HM_DEB=packages/hmailserver_*_${TARGETARCH}.deb",
           "ARG HM_DEB=packages/hmailserver_*_amd64.deb", "names one architecture's .deb")
    broken("the Dockerfile's build that no longer runs the server", "dockerfile", "\n && hmailserver --version\n", "\n && true\n",
           "the server run on the image's platform in the build")
    broken("kaniko building for its own platform alone", "image-build", '  --custom-platform "linux/$arch" \\\n', "",
           "kaniko builds the platform asked for")
    broken("the build taking any architecture", "image-build", "\n  amd64|arm64) ;;\n", "\n  *) ;;\n", "the two platforms, and no other")
    broken("the branch's arm64 image gone", "image", "\nlinux-image-arm64:\n", "\nlinux-image-arm64-old:\n", "has no linux-image-arm64 job")
    broken("the branch's arm64 image from another .deb", "image", "    - job: deb-ubuntu26.04-arm64\n      artifacts: true\n",
           "    - job: deb-ubuntu24.04-arm64\n      artifacts: true\n", "linux-image-arm64 does not need deb-ubuntu26.04-arm64")
    broken("the branch's arm64 image built as amd64", "image", "  variables:\n    HM_IMAGE_ARCH: arm64\n  script:",
           "  variables:\n    HM_IMAGE_ARCH: amd64\n  script:", "linux-image-arm64 does not build the linux/arm64 image")
    broken("the branch's arm64 image allowed to fail", "image", "linux-image-arm64:\n  extends: .own-runners\n",
           "linux-image-arm64:\n  extends: .own-runners\n  allow_failure: true\n", "linux-image-arm64 does not take .own-runners' rules alone")
    broken("release not waiting for the arm64 image", "ci", "    - job: linux-image-arm64\n      artifacts: false\n", "",
           "release does not need linux-image-arm64")
    broken("the tag's arm64 image gone", "image", "\ncontainer-image-arm64:\n", "\ncontainer-image-arm64-old:\n", "has no container-image-arm64 job")
    broken("the tag's arm64 image built as amd64", "image", "    HM_IMAGE_ARCH: arm64\n    HM_IMAGE_DEB_DIR",
           "    HM_IMAGE_ARCH: amd64\n    HM_IMAGE_DEB_DIR", "container-image-arm64 does not build the release's linux/arm64 image")
    broken("a third platform's image", "image", "\ncontainer-image-arm64:\n", "\ncontainer-image-riscv64:\n  stage: release\n\ncontainer-image-arm64:\n",
           "container-image-riscv64 builds an image of a platform")
    broken("a third platform's start", "image", "\ncontainer-image-start-arm64:\n",
           "\ncontainer-image-start-riscv64:\n  stage: release\n\ncontainer-image-start-arm64:\n",
           "container-image-start-riscv64 starts an image of a platform")
    broken("a third platform's scan", "image", "\ncontainer_scanning_arm64:\n",
           "\ncontainer_scanning_riscv64:\n  stage: release\n\ncontainer_scanning_arm64:\n",
           "container_scanning_riscv64 scans an image of a platform")
    broken("the image signed before it has started", "image", "    - job: container-image-start\n      artifacts: false\n", "",
           "container-image-sign does not need container-image-start")
    broken("the deb taken by a name its digest does not hold", "image-deb", '      { grep -Fxq "$deb_sum  $deb" "$out/$sums"',
           '      { true', "each .deb held to the SHA-256 the release's signed sums give its name")
    broken("an SBOM taken without naming its .deb", "image-deb", '      grep -Fq "$deb_sum" "$out/$name" \\\n',
           '      true \\\n', "each SBOM held to naming its .deb's SHA-256")
    broken("the push not checking each image's architecture", "image-push",
           'grep -q "\\"architecture\\":\\"$arch\\"" "$dir/image.json" || fail', 'true || fail',
           "each image refused unless it is for the architecture")
    broken("the push without the arm64 image", "image", "    - job: container-image-arm64\n      artifacts: true\n", "",
           "container-image does not need container-image-arm64")
    broken("the push leaving arm64 out", "image-push", 'platforms="amd64 arm64"', 'platforms="amd64"', "both platforms' images")
    broken("the index made of one platform", "image-push", '--ref "$layout:amd64" --ref "$layout:arm64"', '--ref "$layout:amd64"',
           "naming both platforms' images")
    broken("the push before the index is checked", "image-push", "\nregctl index create ", "\nregctl registry login x; regctl index create ",
           "logs in to the registry before")
    broken("the push without the switch", "image-push", RUNTIME_GATE, 'if false; then', "container-image-push.sh does not refuse")
    broken("the push to the project's first path", "image-push", "\n  registry.gitlab.com/hmailserver/hmailserver) ;;\n",
           "\n  registry.gitlab.com/progressiverobot/hmailserver) ;;\n", "container-image-push.sh does not push to registry.gitlab.com/hmailserver/hmailserver alone")
    broken("the push in regctl by tag alone", "image", "regctl:%s-alpine@sha256:" % regctl, "regctl:%s-alpine@sha25:" % regctl,
           "pinned by digest")
    broken("the push in another regctl than the channels'", "image", "regctl:%s-alpine@" % regctl, "regctl:%s9-alpine@" % regctl,
           "not the release HM_REGCTL_IMAGE pins")
    broken("the push in ghcr.io's regctl", "image", "docker.io/regclient/regctl:%s-alpine@" % regctl,
           "ghcr.io/regclient/regctl:%s-alpine@" % regctl, "does not run in docker.io/regclient/regctl")
    # Artifact Hub: the chart's platforms, and the metadata tag's job, script and file.
    broken("the chart telling Artifact Hub of amd64 alone", "chart", "        - linux/amd64\n        - linux/arm64\n",
           "        - linux/amd64\n", "artifacthub.io/images is not the one image on exactly linux/amd64, linux/arm64")
    broken("the chart naming no platform", "chart", "      platforms:\n        - linux/amd64\n        - linux/arm64\n", "",
           "artifacthub.io/images is not the one image")
    broken("the chart's package held to amd64 alone", "chart-package", 'platforms="linux/amd64 linux/arm64"', 'platforms="linux/amd64"',
           "does not hold the tree's chart and the packaged one to the image's platforms")
    broken("the packaged chart not read back for its platforms", "chart-package", '[ "$(images_block "$work/chart.txt")"', '[ "$(true)"',
           "does not hold the tree's chart and the packaged one to the image's platforms")
    broken("the Artifact Hub job gone", "image", "\ncontainer-image-artifacthub:\n", "\ncontainer-image-artifacthub-old:\n",
           "has no container-image-artifacthub job")
    broken("the Artifact Hub job on the default branch without a press", "image",
           '$CI_COMMIT_REF_PROTECTED == "true"\n      when: manual\n      allow_failure: true\n  script:\n    - sh build/ci/container-image-artifacthub.sh',
           '$CI_COMMIT_REF_PROTECTED == "true"\n      allow_failure: true\n  script:\n    - sh build/ci/container-image-artifacthub.sh',
           "container-image-artifacthub's rules are not .release-rules")
    broken("the Artifact Hub job blocking the default branch", "image",
           "      when: manual\n      allow_failure: true\n  script:\n    - sh build/ci/container-image-artifacthub.sh",
           "      when: manual\n  script:\n    - sh build/ci/container-image-artifacthub.sh", "container-image-artifacthub's rules are not .release-rules")
    broken("the Artifact Hub job on any branch", "image", "    - if: $CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH && $CI_COMMIT_REF_PROTECTED",
           "    - if: $CI_COMMIT_REF_PROTECTED", "container-image-artifacthub's rules are not .release-rules")
    broken("the Artifact Hub job without .release-rules", "image",
           "  rules:\n    - !reference [.release-rules, rules]\n    - if: $CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH",
           "  rules:\n    - if: $CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH", "container-image-artifacthub does not run for a protected v* tag")
    broken("the Artifact Hub job needing container-image outright", "image",
           "    - job: container-image\n      artifacts: false\n      optional: true\n", "    - job: container-image\n      artifacts: false\n",
           "does not need container-image alone, optionally")
    broken("a job waiting for the Artifact Hub job", "image", "    - job: container-image-sbom\n      artifacts: true\n",
           "    - job: container-image-sbom\n      artifacts: true\n    - job: container-image-artifacthub\n", "needs container-image-artifacthub")
    broken("the Artifact Hub job in another regctl", "image",
           "container-image-artifacthub:\n  stage: release\n  image:\n    name: docker.io/regclient/regctl:%s-alpine@" % regctl,
           "container-image-artifacthub:\n  stage: release\n  image:\n    name: docker.io/regclient/regctl:%s9-alpine@" % regctl,
           "container-image-artifacthub does not run in container-image's regctl image")
    broken("the metadata pushed without the switch", "image-artifacthub", RUNTIME_GATE, 'if false; then',
           "container-image-artifacthub.sh does not refuse")
    broken("the metadata pushed to the project's first path", "image-artifacthub", "\n  registry.gitlab.com/hmailserver/hmailserver) ;;\n",
           "\n  registry.gitlab.com/progressiverobot/hmailserver) ;;\n", "container-image-artifacthub.sh does not push to registry.gitlab.com/hmailserver/hmailserver alone")
    broken("the metadata under another tag", "image-artifacthub", "metadata_tag=artifacthub.io", "metadata_tag=latest", "the tag Artifact Hub reads")
    broken("the metadata also written to :latest", "image-artifacthub", '    "$repository:$metadata_tag" ) ||',
           '    "$repository:latest" ) ||', "names a tag other than artifacthub.io")
    broken("the metadata written to ${repository}:latest", "image-artifacthub", '    "$repository:$metadata_tag" ) ||',
           '    "${repository}:latest" ) ||', "names a tag other than artifacthub.io")
    broken("the metadata's tag copied to another", "image-artifacthub", "\nregctl manifest get ",
           '\nregctl image copy "$repository@x" "$CI_REGISTRY_IMAGE:latest"\nregctl manifest get ', "runs regctl image copy")
    broken("a tag of the repository deleted", "image-artifacthub", "manifest_digest=$(regctl image digest ",
           "manifest_digest=$(regctl tag delete ", "runs regctl tag delete")
    broken("the metadata put twice", "image-artifacthub", "\nregctl manifest get ",
           '\n( cd "$work/file" && regctl artifact put --file artifacthub-repo.yml "$repository:$metadata_tag" )\nregctl manifest get ',
           "runs regctl artifact put 2 times")
    broken("the metadata never put", "image-artifacthub", '( cd "$work/file" && regctl artifact put ', '( cd "$work/file" && true ',
           "runs regctl artifact put 0 times")
    broken("the metadata as a layer Artifact Hub does not read", "image-artifacthub",
           "layer_type=application/vnd.cncf.artifacthub.repository-metadata.layer.v1.yaml", "layer_type=application/yaml",
           "the layer Artifact Hub reads")
    broken("the metadata's configuration regctl's own", "image-artifacthub", ' --config-file "$work/empty"', "", "the empty configuration")
    broken("the metadata not read back", "image-artifacthub", 'cmp -s "$out/artifacthub-repo.yml" "$metadata"', "true",
           "held to the file byte for byte")
    broken("the registry password on a command line", "image-artifacthub",
           'printf \'%s\' "$CI_REGISTRY_PASSWORD" | regctl registry login "$CI_REGISTRY" --user "$CI_REGISTRY_USER" --pass-stdin',
           'regctl registry login "$CI_REGISTRY" --user "$CI_REGISTRY_USER" --pass "$CI_REGISTRY_PASSWORD"',
           "hands the registry password to something but regctl's standard input")
    broken("the metadata pushed without an ID", "image-artifacthub", 'if [ "$ids" = 0 ]; then', 'if false; then', "nothing pushed without an ID")
    broken("the metadata file with another key", "image-artifacthub-file", "# here: the script refuses anything else.\n",
           "# here: the script refuses anything else.\nowners:\n", "holds a line that is not a comment")
    broken("the metadata file with an upper-case ID", "image-artifacthub-file", "# here: the script refuses anything else.\n",
           "# here: the script refuses anything else.\nrepositoryID: 0B5EBE9C-0E33-4F5B-9A2D-9A0FBC8A1C55\n", "holds a line that is not a comment")
    broken("the metadata file with two IDs", "image-artifacthub-file", "# here: the script refuses anything else.\n",
           "# here: the script refuses anything else.\nrepositoryID: 0b5ebe9c-0e33-4f5b-9a2d-9a0fbc8a1c55\n"
           "repositoryID: 0b5ebe9c-0e33-4f5b-9a2d-9a0fbc8a1c56\n", "names more than one repository ID")
    broken("the .deb taken for amd64 alone", "image-deb", "for arch in amd64 arm64; do", "for arch in amd64; do", "each platform's .deb")
    broken("the SBOM of amd64 alone", "image-sbom", "platforms=(amd64 arm64)", "platforms=(amd64)", "every platform's SBOM")
    broken("an SBOM that does not name its platform", "image-sbom", '--image "$image" --arch "$arch"', '--image "$image"',
           "each document naming its platform")
    broken("the signature of amd64 alone", "image-sign", 'platforms="amd64 arm64"', 'platforms="amd64"', "every platform's image signed")
    broken("the index left unsigned", "image-sign", '\nsign index "$index"\n', "\ntrue\n", "the index, which every tag names, signed")
    broken("the arm64 scan gone", "image", "\ncontainer_scanning_arm64:\n", "\ncontainer_scanning_old:\n", "has no container_scanning_arm64 job")
    broken("the arm64 scan over the amd64 image", "image", "    HM_SCAN_ARCH: arm64\n", "", "container_scanning_arm64 does not scan")
    broken("the arm64 scan without the image's name", "image", "  dependencies: [container-image]\n  rules:\n",
           "  dependencies: []\n  rules:\n", "container_scanning_arm64 does not scan")

    def broken_image(what, needle, *edits):
        """broken() over the image's CI file with each (old, new) of edits made in turn,
        each old text once in the file as it then is."""
        image = texts["image"]
        for old, new in edits:
            if image.count(old) != 1:
                failures.append("%s: the text to break is not once in image (%r)" % (what, old))
                return
            image = image.replace(old, new, 1)
        broken(what, "image", texts["image"], image, needle)

    def broken_include(what, path, addition, needle, image=None):
        """check() with addition after the included CI file path's text, and the image's
        CI file as image where it is given."""
        if path not in texts["ci-includes"]:
            failures.append("%s: %s is not in the tree" % (what, path))
            return
        copy = dict(texts, **{"ci-includes": dict(texts["ci-includes"], **{path: texts["ci-includes"][path] + addition})})
        if image is not None:
            copy["image"] = image
        found, _ = check(copy, chain, feed)
        if not any(needle in p for p in found):
            failures.append("%s is caught: %s" % (what, found))

    # Either scan's need of container-image without its artifacts, its dependencies line
    # kept: artifacts: false; no, one of YAML's other spellings of false; and a second
    # key, false, after the true one, which GitLab's YAML reader takes. Then a second
    # dependencies key, [], or needs key, without the artifacts, after the job's own,
    # which that reader takes in their place; and a rule that matches in a tag's pipeline
    # with needs of its own, which replace the job's. Each job's entry is found by the
    # key after its dependencies line, the amd64 job's timeout and the arm64 job's rules,
    # and its rules by its HM_SCAN_ARCH after them, which is that job's alone.
    #
    # Then each other form of these GitLab reads, which this checker would read past and
    # by each of which, as GitLab reads it, the job takes no dotenv report from
    # container-image, where the checker of df33e223e let each pass: the fifteen found on
    # 13 October 2026 - a second artifacts, dependencies or needs key, quoted or with a
    # space before its colon; a merge key after the job's own keys, from an anchor in a
    # hidden block before the file's first job; a rule in flow style, with the tag-only
    # condition or another one that matches; rules: !reference [X, rules] on the key's
    # own line, and - !reference [X, rule], X such a hidden block whose rule needs
    # container-image without its artifacts; and the job defined again in
    # .gitlab/ci/snap.yml, the CI file included last - and then a second rules: key, a
    # comment after rules:, a key of a rule quoted or a merge key, the rules indented six,
    # a !reference [X, rules] of a block that only another CI file defines, and a second
    # job: key in its need of container-image, which then names another job while a need
    # in flow style names container-image without its artifacts. Then, once: the job
    # defined again in its own file under a quoted name, whole but for dependencies: [];
    # container_scanning_arm64 without rules of its own, taking container_scanning's,
    # whose second rules key is quoted; .release-rules with a second rules key, bare or
    # with a space before its colon; and a merge key at an included file's top level.
    # And .release-rules defined again in another CI file, which GitLab merges under
    # .gitlab-ci.yml's own and which so keeps the report, is refused as every second
    # definition of what either job reaches is: this reads one.
    first_job = "\nlinux-image-arm64:\n"
    later = ".gitlab/ci/snap.yml"
    own_rules = ("    - if: $CONTAINER_SCANNING_DISABLED == 'true' || $CONTAINER_SCANNING_DISABLED == '1'\n"
                 "      when: never\n    - !reference [.release-rules, rules]\n")
    tag_rule = "    - if: %s\n      needs:\n        - job: container-image\n          artifacts: false\n" % TAG_ONLY
    hidden_rules = '\n.hm-probe:\n  rules:\n    - if: $CI_PIPELINE_SOURCE == "schedule"\n      when: never\n' + tag_rule
    hidden_rule = "\n.hm-probe:\n  rule:\n    if: %s\n    needs:\n      - job: container-image\n        artifacts: false\n" % TAG_ONLY
    for name, arch, after in (("container_scanning", "amd64", "  timeout:"), ("container_scanning_arm64", "arm64", "  rules:")):
        need = "    - job: container-image\n      artifacts: true\n  dependencies: [container-image]\n" + after
        for what, artifacts in (("false", "      artifacts: false\n"), ("no", "      artifacts: no\n"),
                                ("true, then false", "      artifacts: true\n      artifacts: false\n")):
            broken("%s's need of container-image with artifacts %s" % (name, what), "image", need,
                   need.replace("      artifacts: true\n", artifacts, 1), "%s does not need container-image with artifacts: true" % name)
        for key, second in (("dependencies", "  dependencies: []\n"), ("needs", "  needs:\n    - job: container-image\n      artifacts: false\n")):
            broken("%s with a second %s key after its own" % (name, key), "image", need, need.replace(after, second + after, 1),
                   "%s has 2 %s: keys" % (name, key))
        rule = "    - !reference [.release-rules, rules]\n  variables:\n    HM_SCAN_ARCH: %s\n" % arch
        broken("%s with a rule that brings needs of its own" % name, "image", rule,
               "    - if: $CI_COMMIT_TAG\n      needs:\n        - job: container-image\n          artifacts: false\n" + rule,
               "%s takes a rule with needs of its own" % name)
        unread = "%s is written in a form this checker does not read" % name
        variables = "  variables:\n    HM_SCAN_ARCH: %s\n" % arch
        rules = "  rules:\n" + own_rules + variables
        cases = [("a second artifacts key, %s" % spelled,
                  [(need, need.replace("      artifacts: true\n", "      artifacts: true\n      %s\n" % spelled, 1))])
                 for spelled in ('"artifacts": false', "artifacts : false", "'artifacts': false")]
        cases += [("a second %s key, %s" % (key, spelled.split("\n")[0]), [(need, need.replace(after, "  %s\n" % spelled + after, 1))])
                  for key, spelled in (("dependencies", '"dependencies": []'), ("dependencies", "dependencies : []"),
                                       ("dependencies", "'dependencies': []"),
                                       ("needs", '"needs":\n    - job: container-image\n      artifacts: false'),
                                       ("needs", "needs : [{job: container-image, artifacts: false}]"))]
        cases += [("a merge key after its own keys, bringing %s" % brought.split(":")[0],
                   [(first_job, "\n.hm-probe:\n  parts: &hm-probe\n    %s\n" % brought + first_job),
                    (need, need.replace(after, "  <<: *hm-probe\n" + after, 1))])
                  for brought in ("dependencies: []", "needs:\n      - job: container-image\n        artifacts: false")]
        cases += [("a rule in flow style, if: %s" % condition,
                   [(rule, "    - {if: %s, needs: [{job: container-image, artifacts: false}]}\n" % condition + rule)])
                  for condition in ("$CI_COMMIT_TAG", "'%s'" % TAG_ONLY)]
        cases += [("rules: !reference [X, rules] on the key's own line",
                   [(first_job, hidden_rules + first_job), (rules, "  rules: !reference [.hm-probe, rules]\n" + variables)]),
                  ("a rule by - !reference [X, rule]",
                   [(first_job, hidden_rule + first_job), (rule, "    - !reference [.hm-probe, rule]\n" + rule)]),
                  ("a comment after rules:, before a rule with needs",
                   [(rules, "  rules: # the scan's\n" + tag_rule + own_rules + variables)]),
                  ("a rule's needs key quoted", [(rule, tag_rule.replace("      needs:\n", '      "needs":\n', 1) + rule)]),
                  ("a rule's needs by a merge key",
                   [(rule, "    - if: %s\n      <<: {needs: [{job: container-image, artifacts: false}]}\n" % TAG_ONLY + rule)]),
                  ("its rules indented six",
                   [(rules, "  rules:\n" + "".join("  %s\n" % line for line in (tag_rule + own_rules).split("\n")[:-1]) + variables)])]
        for what, edits in cases:
            broken_image("%s with %s" % (name, what), unread, *edits)
        broken_image("%s with a second rules: key" % name, "%s has 2 rules: keys" % name, (rule, rule + "  rules:\n" + tag_rule))
        broken_image("%s with a second job key in its need of container-image, which a need in flow style then names" % name,
                     "%s's need of container-image has 2 job: keys" % name,
                     (need, need.replace("      artifacts: true\n", "      artifacts: true\n      job: container-image-sbom\n"
                                         "    - {job: container-image, artifacts: false}\n", 1)))
        broken_include("%s defined again in %s" % (name, later), later, "\n%s:\n  dependencies: []\n" % name,
                       "%s is defined in" % name)
        broken_include("%s with a rule by !reference [X, rules], X a block %s alone defines" % (name, later), later, hidden_rules,
                       unread, texts["image"].replace(rule, "    - !reference [.hm-probe, rules]\n" + rule, 1))
    broken("container_scanning defined again in its own file, under a quoted name", "image", texts["image"],
           texts["image"] + '\n"container_scanning":\n' + blocks(texts["image"]).get("container_scanning", "").replace(
               "  dependencies: [container-image]\n", "  dependencies: []\n", 1) + "\n", "container_scanning is defined twice in")
    broken_image("container_scanning_arm64 taking container_scanning's rules, whose second rules key is quoted",
                 'container_scanning_arm64 is written in a form this checker does not read (container_scanning: "rules":)',
                 ("  dependencies: [container-image]\n  rules:\n" + own_rules, "  dependencies: [container-image]\n"),
                 ("    - !reference [.release-rules, rules]\n  variables:\n    HM_SCAN_ARCH: amd64\n",
                  "    - !reference [.release-rules, rules]\n  variables:\n    HM_SCAN_ARCH: amd64\n  \"rules\":\n" + tag_rule))
    release_rules = (".release-rules:\n  rules:\n    - if: %s || $CI_PIPELINE_SOURCE == \"merge_request_event\"\n      when: never\n"
                     "    - if: %s\n" % (REFUSAL, TAG_ONLY))
    broken(".release-rules with a second rules key, a space before its colon", "ci", release_rules,
           release_rules + "  rules :\n" + tag_rule,
           "container_scanning is written in a form this checker does not read (the rules it takes, in .release-rules: rules :)")
    broken(".release-rules with a second rules key", "ci", release_rules, release_rules + "  rules:\n" + tag_rule,
           "container_scanning is written in a form this checker does not read (the rules it takes, in .release-rules: a second "
           "rules: key)")
    broken_include("a template both scans take defined again", later, "\n.release-rules:\n  rules:\n    - when: never\n",
                   ".release-rules is defined in")
    broken_include("a merge key at an included file's top level", later, "\n<<: {container_scanning: {dependencies: []}}\n",
                   "%s has a line at the top level that is not a key" % later)
    broken("the arm64 start gone", "image", "\ncontainer-image-start-arm64:\n", "\ncontainer-image-start-old:\n",
           "has no container-image-start-arm64 job")
    broken("the arm64 start as the host's platform", "image", "      docker:\n        platform: linux/arm64\n", "",
           "container-image-start-arm64 does not start the linux/arm64 image")
    broken("the arm64 start on the index", "image", "    - name: $HM_IMAGE_ARM64\n", "    - name: $HM_IMAGE\n",
           "container-image-start-arm64 does not start the linux/arm64 image")
    # :latest and :<major>.<minor> only forward.
    broken(":latest moved back to an older version", "image-push", '      older) keep "$t"', '      older) move "$t"',
           "never moved back to an older version")
    broken("versions compared the wrong way", "image-push", 'if [ "${x:-0}" -gt "${y:-0}" ]; then',
           'if [ "${x:-0}" -lt "${y:-0}" ]; then', "does not compare versions number by number")
    broken("a tag moved without reading what it names", "image-push", 'regctl image config "$repository:$t"',
           'regctl image digest "$repository:$t"', "the version the tag names now")
    broken("a tag whose version cannot be read moved anyway", "image-push",
           '      keep "$t" "the image it names has no version this job can read',
           '      move "$t" "the image it names has no version this job can read', "whose version cannot be read left")
    broken("a tag put on the index outside move()", "image-push", '\nwritten="$version"\n',
           '\nregctl image copy "$repository@$index" "$repository:latest"\nwritten="$version"\n', "outside move()")
    # A file the checker reads, missing from the tree: a problem, not a traceback.
    held = FILES["image-push"]
    try:
        FILES["image-push"] = os.path.join("build", "ci", "no-such-push-script.sh")
        found, _ = run_checks(load_texts(), chain, feed)
        if not any("build/ci/no-such-push-script.sh is not in the tree" in p for p in found):
            failures.append("a file missing from the tree is reported: %s" % found)
    except Exception as error:
        failures.append("a file missing from the tree is reported, not raised: %s: %s" % (type(error).__name__, error))
    finally:
        FILES["image-push"] = held
    # The security scans, before the release.
    broken("a release that does not wait for GitLab's SAST", "ci", "    - job: semgrep-sast\n      artifacts: true\n", "",
           "release does not need semgrep-sast")
    broken("a release that does not bring the dependency scan's report", "ci", "    - job: dependency-scan\n      artifacts: true\n",
           "    - job: dependency-scan\n      artifacts: false\n", "release does not bring dependency-scan's artifacts")
    broken("the source scan allow_failure again", "ci", "  timeout: 120 minutes\n  script:\n    - bash build/ci/static-analysis.sh\n",
           "  timeout: 120 minutes\n  allow_failure: true\n  script:\n    - bash build/ci/static-analysis.sh\n",
           "static-analysis may fail without failing the pipeline")
    broken("semgrep allowed to fail on some exit codes", "ci", "  timeout: 30 minutes\n  script:\n    - bash build/ci/semgrep.sh\n",
           "  timeout: 30 minutes\n  allow_failure:\n    exit_codes: [1]\n  script:\n    - bash build/ci/semgrep.sh\n",
           "semgrep may fail without failing the pipeline")
    broken("semgrep allowed to fail in YAML 1.1's other spelling of true", "ci",
           "  timeout: 30 minutes\n  script:\n    - bash build/ci/semgrep.sh\n",
           "  timeout: 30 minutes\n  allow_failure: yes\n  script:\n    - bash build/ci/semgrep.sh\n",
           "semgrep may fail without failing the pipeline")
    broken("a release that needs a scan optionally", "ci", "    - job: static-analysis\n      artifacts: true\n",
           "    - job: static-analysis\n      artifacts: true\n      optional: true\n", "release needs static-analysis optionally")
    broken("GitLab's SAST not run for a release tag", "security",
           '    - if: $CI_COMMIT_TAG =~ /^v/ && $CI_COMMIT_REF_PROTECTED == "true"\n  # The analyzer writes its report',
           "  # The analyzer writes its report", "semgrep-sast does not run for a protected v* tag")
    broken("GitLab's SAST report kept as a report only", "security", "    paths:\n      - gl-sast-report.json\n", "",
           "semgrep-sast does not keep gl-sast-report.json")
    broken("collect not asking the gate", "chain",
           '        problems.extend(security_gate.evaluate(root, root, exceptions_text=exceptions or "exceptions: []\\n", say=say,\n'
           '                                               not_before=security_gate.pipeline_created(env)))\n', "",
           "no longer asks build/ci/security-gate.py")
    broken("the gate given the tagged tree's exceptions", "chain", 'exceptions_text=exceptions or "exceptions: []\\n", say=say,',
           "say=say,", "does not give the gate master's")
    broken("collect taking a committed report", "chain", "        problems.extend(security_gate.committed_reports(root, run))\n",
           "", "does not refuse a tagged tree that commits a scan's report")
    broken("collect taking an older report", "chain", ",\n                                               not_before=security_gate.pipeline_created(env)",
           "", "does not refuse a tagged tree that commits a scan's report")
    # semgrep-sast's report is its own, and nothing narrows it unseen.
    broken("semgrep-sast allowed to fail again", "security", "  allow_failure: false\n  variables:\n",
           "  allow_failure: true\n  variables:\n", "semgrep-sast does not say allow_failure: false")
    broken("semgrep-sast keeping a report the checkout carries", "security", "    - rm -f gl-sast-report.json semgrep.sarif\n", "",
           "semgrep-sast does not remove gl-sast-report.json and semgrep.sarif")
    broken("semgrep-sast not keeping its SARIF", "security", "      - semgrep.sarif\n", "", "does not keep semgrep.sarif")
    broken("an exclusion at any depth", "security", '"/spec, ', '"spec, ', "an entry with no slash leaves out")
    broken("semgrep-sast's second list of exclusions left to the CI files", "security", '    SAST_SEMGREP_EXCLUDED_PATHS: ""\n', "",
           "does not set SAST_SEMGREP_EXCLUDED_PATHS to nothing itself")
    for name in SAST_NARROWING_VARIABLES:
        broken("%s set for every job" % name, "ci", '  PYTHONUTF8: "1"\n', '  PYTHONUTF8: "1"\n  %s: "x"\n' % name,
               ".gitlab-ci.yml defines %s" % name)
    broken("another list of what semgrep-sast leaves out", "ci", '  PYTHONUTF8: "1"\n',
           '  PYTHONUTF8: "1"\n  SAST_EXCLUDED_PATHS: "hmailserver"\n', ".gitlab-ci.yml defines SAST_EXCLUDED_PATHS")
    for name in ("CI_MERGE_REQUEST_DIFF_BASE_SHA", "CI_PIPELINE_SOURCE", "GITLAB_FEATURES"):
        broken("a CI file that defines %s" % name, "ci", '  PYTHONUTF8: "1"\n', '  PYTHONUTF8: "1"\n  %s: "x"\n' % name,
               ".gitlab-ci.yml defines %s, a variable GitLab gives" % name)
    for planted in (".semgrepignore", "hmailserver/source/.semgrepignore", ".semgrepignore.local", ".gitlab/sast-ruleset.toml",
                    ".gitlab/secret-detection-ruleset.toml",
                    "gl-sast-report.json", "semgrep.sarif", "security/semgrep/gl-sast-semgrep.json"):
        found, _ = check(dict(texts, tree=list(texts["tree"]) + [planted]), chain, feed)
        if not any("the tree carries %s" % planted in p for p in found):
            failures.append("a tree that carries %s is caught: %s" % (planted, found))
    workflow_rule = WORKFLOW_BATCH_RULE.split("\n")[0]
    broken("a fork's protected branch running no merge request pipeline", "ci", workflow_rule,
           workflow_rule.replace("$CI_MERGE_REQUEST_PROJECT_ID", "$CI_PROJECT_ID"), "first workflow rule is not")
    broken("every protected branch's merge request running none", "ci", workflow_rule,
           workflow_rule.replace(" && $CI_MERGE_REQUEST_SOURCE_BRANCH_NAME =~ /^batch/", ""), "first workflow rule is not")
    broken("the gates run for more branches than the workflow names", "mr-checks", GATE_RULES[1],
           GATE_RULES[1].replace("/^batch/", "/^(batch|release)/"), "are not the ones the gates run for")
    # The change gates, on every change: semgrep-sast where its gate needs it, the gates'
    # rules, retries and bases, the scans on linix against master, and land-via-mr's list.
    broken("semgrep-sast not run on a batch* branch", "security", "    " + GATE_SCAN_BATCH_RULE + "\n", "",
           "semgrep-sast does not run on a protected batch* branch")
    broken("semgrep-sast switched off by SAST_DISABLED again", "security",
           "  rules:\n    - !reference [.gitlab-security-rules, rules]\n",
           "  rules:\n    - if: $SAST_DISABLED == 'true' || $SAST_DISABLED == '1'\n      when: never\n"
           "    - !reference [.gitlab-security-rules, rules]\n", "semgrep-sast's rules read SAST_DISABLED")
    mirrored = MIRRORED_SAST.search(texts["security"])
    pinned = mirrored.group(0) if mirrored else "the mirrored semgrep image is not found"
    broken("semgrep-sast from GitLab's registry again", "security", pinned,
           "    name: registry.gitlab.com/security-products/semgrep:6@sha256:%s" % (mirrored.group(2) if mirrored else ""),
           "does not run GitLab's analyzer as the project's registry holds it")
    broken("the mirror's tag naming another digest", "security", pinned,
           pinned.replace("-%s@" % (mirrored.group(1) if mirrored else ""), "-000000000000@"), "the copy's tag is the digest's")
    broken("a gate not retried when a runner fails", "security", "      - runner_system_failure\n", "",
           ".gate-retry (.gitlab/ci/gitlab-security.yml) does not retry twice on runner_system_failure")
    broken("a gate retried on the reason GitLab split", "security", "      - no_updates_running\n",
           "      - stuck_or_timeout_failure\n", "names stuck_or_timeout_failure")
    broken("semgrep-sast without the gates' retry", "security", "  retry: !reference [.gate-retry, retry]\n", "",
           "semgrep-sast does not take .gate-retry's retry")
    broken("the gates behind a list of changes again", "mr-checks", '    - if: $CI_PIPELINE_SOURCE == "merge_request_event"\n',
           '    - if: $CI_PIPELINE_SOURCE == "merge_request_event"\n      changes:\n        paths:\n'
           '          - "**/packages*.lock.json"\n', "rules have a changes: list")
    broken("a gate allowed to fail", "mr-checks", "mr-dependency-check:\n  extends: .mr-check\n",
           "mr-dependency-check:\n  extends: .mr-check\n  allow_failure: true\n",
           "mr-dependency-check may fail without failing the pipeline")
    broken("the gates not on a batch* branch", "mr-checks", "    " + GATE_RULES[1] + "\n", "",
           "does not run for every change")
    broken("a merge request's gates narrowed by a variable", "mr-checks", '    - if: $CI_PIPELINE_SOURCE == "merge_request_event"\n',
           '    - if: $CI_PIPELINE_SOURCE == "merge_request_event" && $HM_GATES == "on"\n', "does not run for every change")
    broken("the gates switched off by SAST_DISABLED", "mr-checks", '    - if: $CI_PIPELINE_SOURCE == "merge_request_event"\n',
           "    - if: $SAST_DISABLED == 'true'\n      when: never\n"
           '    - if: $CI_PIPELINE_SOURCE == "merge_request_event"\n', "semgrep-sast-gate's rules read SAST_DISABLED")
    broken("the dependency gate given the merge request's base alone", "mr-checks",
           "    - python3 build/ci/mr-dependency-check.py --base auto\n",
           '    - python3 build/ci/mr-dependency-check.py --base "$CI_MERGE_REQUEST_DIFF_BASE_SHA"\n',
           "mr-dependency-check does not run python3 build/ci/mr-dependency-check.py --base auto")
    broken("the gates not retried on exit 2", "mr-checks", "      - 2\n      - 100\n", "      - 100\n",
           "is not retried twice on its script's exit 2")
    broken("the SAST gate without semgrep-sast's report", "mr-checks", "    - job: semgrep-sast\n      artifacts: true\n",
           "    - job: semgrep-sast\n      artifacts: false\n", "semgrep-sast-gate does not need semgrep-sast with its artifacts")
    broken("the scans on a batch compared with the push before", "ci", "        compare_to: refs/heads/master\n", "",
           "dependency-scan does not compare a protected batch* branch with master")
    broken("a scan back on .own-runners' rules", "ci",
           "static-analysis:\n  extends: .linix\n  rules:\n    - !reference [.scans-on-the-whole-change, rules]\n",
           "static-analysis:\n  extends: .linix\n", "static-analysis does not compare a protected batch* branch with master")
    broken("a docs-only batch that would scan by its last push", "ci",
           "    - if: $CI_COMMIT_BRANCH =~ /^batch/\n      when: never\n", "",
           "semgrep does not compare a protected batch* branch with master")
    gate_jobs = re.search(r"^GATE_JOBS = .*$", texts["land"], re.MULTILINE)
    gate_jobs = gate_jobs.group(0) if gate_jobs else "GATE_JOBS not found"
    scan_jobs = re.search(r"^SOURCE_SCAN_JOBS = .*$", texts["land"], re.MULTILINE)
    scan_jobs = scan_jobs.group(0) if scan_jobs else "SOURCE_SCAN_JOBS not found"
    broken("land-via-mr requiring a job no pipeline has", "land", gate_jobs, gate_jobs.replace(")", ', "no-such-job")'),
           "GATE_JOBS requires no-such-job, which no job")
    broken("land-via-mr not requiring a change gate", "land", gate_jobs, 'GATE_JOBS = ("semgrep-sast", "semgrep-sast-gate")',
           "GATE_JOBS does not require mr-dependency-check")
    broken("land-via-mr not requiring semgrep-sast", "land", gate_jobs,
           'GATE_JOBS = ("semgrep-sast-gate", "mr-dependency-check")', "GATE_JOBS does not require semgrep-sast")
    broken("land-via-mr not requiring a blocking scan", "land", scan_jobs, 'SOURCE_SCAN_JOBS = ("static-analysis",)',
           "SOURCE_SCAN_JOBS does not require semgrep")
    broken("land-via-mr without its list of scans", "land", scan_jobs, "SCANS = ()", "names no SOURCE_SCAN_JOBS")
    broken("land-via-mr's gates named by an expression", "land", gate_jobs, "GATE_JOBS = tuple(sorted(OWNER))",
           "GATE_JOBS is not a literal list of job names")
    broken("land-via-mr not requiring checkers", "land", gate_jobs,
           'GATE_JOBS = ("semgrep-sast", "semgrep-sast-gate", "mr-dependency-check")', "GATE_JOBS does not require checkers")
    landing = landing_module()
    files_held = tuple(landing.GATE_FILES)
    for constant, stand_in, needle in (
            ("GATE_FILES", ("build/land-via-mr.py",), "GATE_FILES leave out"),
            ("GATE_FILES", tuple(f for f in files_held if f != ".gitlab-ci.yml"), "GATE_FILES leave out .gitlab-ci.yml"),
            ("GATE_FILES", tuple(f for f in files_held if f != ".gitlab/ci/no-github.yml"),
             "GATE_FILES leave out .gitlab/ci/no-github.yml"),
            ("GATE_FILES", tuple(f for f in files_held if f != "build/scan-secrets.py"),
             "GATE_FILES leave out build/scan-secrets.py"),
            ("GATE_FILES", tuple(f for f in files_held if f != "build/ci/secret-detection-rules.sh"),
             "GATE_FILES leave out build/ci/secret-detection-rules.sh"),
            ("GATE_FILES", tuple(f for f in files_held if f != "build/ci/semgrep.sh"),
             "GATE_FILES leave out build/ci/semgrep.sh"),
            ("GATE_FILES", tuple(f for f in files_held if f != "build/ci/static-analysis.sh"),
             "GATE_FILES leave out build/ci/static-analysis.sh"),
            ("GATE_FILES", files_held + ("build/ci/release-chain.py",),
             "build/ci/release-chain.py does not keep its own directory off sys.path"),
            ("CHECKOUT_COPIES", tuple(f for f in landing.CHECKOUT_COPIES if f != ".gitlab/ci/gitlab-security.yml"),
             "CHECKOUT_COPIES leave out .gitlab/ci/gitlab-security.yml"),
            ("CHECKOUT_COPIES", tuple(f for f in landing.CHECKOUT_COPIES if f != "build/scan-secrets.py"),
             "CHECKOUT_COPIES leave out build/scan-secrets.py"),
            ("CHECKOUT_COPIES", None, "CHECKOUT_COPIES leave out"),
            ("GATE_CODE", tuple(landing.GATE_CODE)[:-1], "GATE_CODE is not security-gate.py's"),
            ("BASE_GATES", ("semgrep-sast-gate",), "BASE_GATES are not the change gates"),
            ("BASE_COPY_SCRIPTS", {"semgrep-sast-gate": "build/ci/security-findings.py",
                                   "mr-dependency-check": "build/ci/mr-dependency-check.py"}, "BASE_COPY_SCRIPTS are not"),
            ("BASE_COPY_SCRIPTS", {"semgrep-sast-gate": "build/ci/security-gate.py"}, "BASE_COPY_SCRIPTS are not")):
        held = getattr(landing, constant)
        try:
            setattr(landing, constant, stand_in)
            found, _ = check(texts, chain, feed)
        finally:
            setattr(landing, constant, held)
        if not any(needle in p for p in found):
            failures.append("land-via-mr's %s as %r is caught: %s" % (constant, stand_in, found))
    # The files a gate job reaches with extends or !reference are the gates' own: one
    # that takes a template from another CI file brings that file in.
    expect_files = {".gitlab/ci/mr-checks.yml", ".gitlab/ci/gitlab-security.yml", ".gitlab/ci/no-github.yml"}
    if not expect_files <= gate_job_files(texts):
        failures.append("the gate jobs' files are found from their extends and !reference: %s" % sorted(gate_job_files(texts)))
    elsewhere = sorted((path, name) for path, text in texts["ci-includes"].items() if path not in expect_files
                       for name in blocks(text) if name.startswith("."))
    if elsewhere:
        path, template = elsewhere[0]
        broken("a gate job taking a template from another CI file", "mr-checks", "semgrep-sast-gate:\n  extends: .mr-check\n",
               "semgrep-sast-gate:\n  extends: [.mr-check, %s]\n" % template, "GATE_FILES leave out %s" % path)
        broken("a gate job taking a part of another CI file's template", "mr-checks",
               "    - !reference [.no-github, before_script]\n",
               "    - !reference [.no-github, before_script]\n    - !reference [%s, script]\n" % template,
               "GATE_FILES leave out %s" % path)
    else:
        failures.append("no CI file of the tree has a template of its own to break the gate jobs' files with")
    # No CI file but a job's own defines a job that holds a change, or a template one
    # reaches: GitLab merges a second definition into the first. No CI file but
    # .gitlab-ci.yml sets what every job takes, or includes anything but GitLab's templates.
    if ci_override_problems(texts):
        failures.append("the tree's CI files define a job that holds a change once: %s" % ci_override_problems(texts))
    named_files = set(relative.replace(os.sep, "/") for relative in FILES.values())
    spare = [p for p in sorted(texts["ci-includes"]) if p not in named_files and p not in expect_files]
    if not spare:
        failures.append("no CI file of the tree is free to break the jobs' definitions with")
    for what, addition, needle in (
            ("a change gate defined again in another CI file", "\nsemgrep-sast-gate:\n  allow_failure: true\n",
             "semgrep-sast-gate is defined in"),
            ("a change gate defined again in a flow mapping", "\nmr-dependency-check: {rules: [{when: never}]}\n",
             "mr-dependency-check is defined in"),
            ("the gates' scan defined again under a quoted name", "\n\"semgrep-sast\":\n  allow_failure: true\n",
             "semgrep-sast is defined in"),
            ("checkers defined again", "\ncheckers:\n  allow_failure: true\n", "checkers is defined in"),
            ("a blocking scan defined again", "\nstatic-analysis:\n  when: manual\n", "static-analysis is defined in"),
            ("a template a gate extends defined again", "\n.mr-check:\n  allow_failure: true\n", ".mr-check is defined in"),
            ("a template a gate takes a part of defined again", "\n.gate-retry:\n  retry:\n    max: 0\n",
             ".gate-retry is defined in"),
            ("variables every job takes, in an included file", "\nvariables:\n  PYTHONPATH: build/x\n",
             "sets variables at its top level"),
            ("defaults every job takes, in an included file", "\ndefault:\n  hooks:\n    pre_get_sources_script: [\"true\"]\n",
             "sets default at its top level"),
            ("a workflow in an included file", "\nworkflow:\n  rules:\n    - when: always\n", "sets workflow at its top level"),
            ("an included file including a remote one", "\ninclude:\n  - remote: https://example.invalid/x.yml\n",
             "includes what is not one of GitLab's templates"),
            ("an included file including another of the tree", "\ninclude:\n  - local: .gitlab/ci/x.yml\n",
             "includes what is not one of GitLab's templates"),
            ("an included file including on one line", "\ninclude: https://example.invalid/x.yml\n",
             "includes what is not one of GitLab's templates"),
            ("an included file including one of GitLab's templates", "\ninclude:\n  - template: Jobs/Dependency-Scanning.gitlab-ci.yml\n",
             None)):
        if not spare:
            break
        copy = dict(texts, **{"ci-includes": dict(texts["ci-includes"], **{spare[-1]: texts["ci-includes"][spare[-1]] + addition})})
        found, _ = check(copy, chain, feed)
        overriding = [p for p in found if " is defined in " in p or " at its top level" in p or " includes what " in p]
        if (needle and not any(needle in p for p in found)) or (needle is None and overriding):
            failures.append("%s in %s is %s: %s" % (what, spare[-1], "caught" if needle else "let pass", overriding or found))
    # Every job on the project's own runners for a protected ref that is not a merge
    # request's, and on a runner that takes it in any other pipeline (runner_problems):
    # the tree passes, and each way out below is caught - in .gitlab-ci.yml, or in an
    # included file no key of FILES names.
    found, _ = runner_problems(texts)
    if found:
        failures.append("the tree's jobs all run on the project's runners: %s" % found)
    editorconfig = "    - apk add --no-cache -q editorconfig-checker\n"
    channels = ".gitlab/ci/channels.yml"
    for what, path, old, new, needle in (
            ("default: without the runner", ".gitlab-ci.yml", "  tags: [$HM_RUNNER]\n", "", "default: names no tags"),
            ("default: naming linix itself", ".gitlab-ci.yml", "  tags: [$HM_RUNNER]\n", "  tags: [linix]\n",
             "not tags: [$HM_RUNNER]"),
            ("HM_RUNNER GitLab's runner for every pipeline", ".gitlab-ci.yml", "  HM_RUNNER: linix\n",
             "  HM_RUNNER: saas-linux-small-amd64\n", "it must be linix"),
            ("HM_RUNNER set nowhere", ".gitlab-ci.yml", "  HM_RUNNER: linix\n", "", "do not set HM_RUNNER"),
            ("a merge request's pipeline left on linix", ".gitlab-ci.yml",
             '    - if: $CI_PIPELINE_SOURCE == "merge_request_event"\n      variables:\n        HM_RUNNER: saas-linux-small-amd64\n',
             '    - if: $CI_PIPELINE_SOURCE == "merge_request_event"\n', "makes a pipeline for a merge request"),
            ("an unprotected branch's pipeline left on linix", ".gitlab-ci.yml",
             "    - if: $CI_COMMIT_BRANCH\n      variables:\n        HM_RUNNER: saas-linux-small-amd64\n",
             "    - if: $CI_COMMIT_BRANCH\n", "makes a pipeline for an unprotected ref"),
            ("an unprotected v* tag's pipeline left on linix", ".gitlab-ci.yml",
             "    - if: $CI_COMMIT_TAG =~ /^v/\n      variables:\n        HM_RUNNER: saas-linux-small-amd64\n",
             "    - if: $CI_COMMIT_TAG =~ /^v/\n", "makes a pipeline for an unprotected ref"),
            ("a protected branch's pipeline sent to GitLab's runner", ".gitlab-ci.yml",
             '    - if: $CI_COMMIT_BRANCH && $CI_COMMIT_REF_PROTECTED == "true"\n    - if: $CI_COMMIT_BRANCH\n',
             "    - if: $CI_COMMIT_BRANCH\n", "would go to GitLab's hosted runners"),
            ("a protected v* tag's pipeline sent to GitLab's runner", ".gitlab-ci.yml",
             '    - if: $CI_COMMIT_TAG =~ /^v/ && $CI_COMMIT_REF_PROTECTED == "true"\n    - if: $CI_COMMIT_TAG =~ /^v/\n',
             "    - if: $CI_COMMIT_TAG =~ /^v/\n", "would go to GitLab's hosted runners"),
            ("a workflow rule this cannot read", ".gitlab-ci.yml", "workflow:\n  rules:\n",
             'workflow:\n  rules:\n    - if: $CI_COMMIT_BRANCH = "x"\n      when: never\n', "workflow: is not read here"),
            ("a job that sets HM_RUNNER itself", ".gitlab-ci.yml", editorconfig,
             editorconfig + "  variables:\n    HM_RUNNER: saas-linux-small-amd64\n", "sets HM_RUNNER in editorconfig"),
            ("a job that sets HM_RUNNER in a flow mapping", ".gitlab-ci.yml", editorconfig,
             editorconfig + "  variables: {HM_RUNNER: saas-linux-small-amd64}\n", "sets HM_RUNNER in editorconfig"),
            ("a job with no tags", ".gitlab-ci.yml", editorconfig, editorconfig + "  tags: []\n",
             "editorconfig (.gitlab-ci.yml) names no tags"),
            ("a job on GitLab's runner by name", ".gitlab-ci.yml", editorconfig,
             editorconfig + "  tags: [saas-linux-small-amd64]\n", "which no runner of the project's carries all of"),
            ("a job on a tag GitLab's runners could carry", ".gitlab-ci.yml", editorconfig, editorconfig + "  tags: [docker]\n",
             "none of which only the project's runners carry"),
            ("a job on a variable of its own", ".gitlab-ci.yml", editorconfig, editorconfig + "  tags: [$PROBE_RUNNER]\n",
             "by a variable other than HM_RUNNER"),
            ("a job on tags by an alias", ".gitlab-ci.yml", editorconfig, editorconfig + "  tags: *probe-tags\n",
             "names its tags in a form this does not read"),
            ("a job leaving default:", ".gitlab-ci.yml", editorconfig, editorconfig + "  inherit:\n    default: false\n",
             "says inherit:"),
            ("a check of every pipeline on linix's tag, merge requests' included", ".gitlab-ci.yml", editorconfig,
             editorconfig + "  tags:\n    - docker\n    - linix\n", "makes it in a pipeline for a merge request"),
            ("the project's jobs made for a merge request", ".gitlab-ci.yml",
             '    - if: $CI_PIPELINE_SOURCE != "merge_request_event" && $CI_COMMIT_REF_PROTECTED == "true"\n      changes:\n'
             '        paths: &not-docs-only\n',
             '    - if: $CI_COMMIT_REF_PROTECTED == "true" || $CI_PIPELINE_SOURCE == "merge_request_event"\n      changes:\n'
             '        paths: &not-docs-only\n', "makes it in a pipeline for a merge request"),
            ("the release's jobs made for an unprotected v* tag", ".gitlab-ci.yml",
             '      when: never\n    - if: $CI_COMMIT_TAG =~ /^v/ && $CI_COMMIT_REF_PROTECTED == "true"\n\n# The installer',
             '      when: never\n    - if: $CI_COMMIT_TAG =~ /^v/\n\n# The installer', "makes it in a pipeline for an unprotected ref"),
            ("an included job with no tags", channels, "", "\nprobe-job:\n  tags: []\n  script: [echo]\n",
             "probe-job (.gitlab/ci/channels.yml) names no tags"),
            ("an included job on linix for every pipeline", channels, "", "\nprobe-job:\n  tags: [linix]\n  script: [echo]\n",
             "probe-job (.gitlab/ci/channels.yml) is tagged ['linix']"),
            ("a check put on linix by a second definition in an included file", channels, "", "\neditorconfig:\n  tags: [linix]\n",
             "editorconfig (.gitlab-ci.yml and .gitlab/ci/channels.yml) is tagged ['linix']"),
            ("an included job on extends as a block list", channels, "", "\nprobe-job:\n  extends:\n    - .linix\n  script: [echo]\n",
             "gives extends: as a block list"),
            ("a template of the project's jobs on a tag any runner may carry", ".gitlab-ci.yml",
             ".linix:\n  extends: .own-runners\n  tags: [$HM_RUNNER_BUILD]\n", ".linix:\n  extends: .own-runners\n  tags: [docker]\n",
             "linux-build (.gitlab-ci.yml) names the tags ['docker']"),
            ("an included job on linix whose merge requests are sent to never only when a file changed", channels, "",
             '\nprobe-job:\n  tags: [linix]\n  rules:\n    - if: $CI_PIPELINE_SOURCE == "merge_request_event"\n'
             '      changes: [README.md]\n      when: never\n    - if: $CI_COMMIT_REF_PROTECTED == "true"\n  script: [echo]\n',
             "probe-job (.gitlab/ci/channels.yml) is tagged ['linix'], for a runner of the project's, and its rule 2"),
            ("an included job on linix whose merge requests are sent to never first", channels, "",
             '\nprobe-job:\n  tags: [linix]\n  rules:\n    - if: $CI_PIPELINE_SOURCE == "merge_request_event"\n'
             '      when: never\n    - if: $CI_COMMIT_REF_PROTECTED == "true"\n  script: [echo]\n', None),
            ("an included job taking default:'s tags", channels, "", "\nprobe-job:\n  script: [echo]\n", None)):
        copy = dict(texts)
        if path == ".gitlab-ci.yml":
            if old not in copy["ci"]:
                failures.append("%s: the text to break is not in .gitlab-ci.yml (%r)" % (what, old))
                continue
            copy["ci"] = copy["ci"].replace(old, new, 1)
        else:
            if path not in copy["ci-includes"]:
                failures.append("%s: %s is not in the tree" % (what, path))
                continue
            copy["ci-includes"] = dict(copy["ci-includes"], **{path: copy["ci-includes"][path] + new})
        found, _ = runner_problems(copy)
        if needle is None and any("probe-job" in p for p in found):
            failures.append("%s is let pass: %s" % (what, [p for p in found if "probe-job" in p]))
        elif needle is not None and not any(needle in p for p in found):
            failures.append("%s is caught: %s" % (what, found))
    copy = dict(texts, ci=texts["ci"].replace(editorconfig, editorconfig + "  tags: [$HM_RUNNER]\n", 1))
    copy["ci-includes"] = dict(texts["ci-includes"], **{channels: texts["ci-includes"].get(channels, "") +
                                                        "\neditorconfig:\n  tags: [$HM_RUNNER]\n"})
    found, _ = runner_problems(copy)
    if not any("editorconfig names its tags in" in p for p in found):
        failures.append("a job naming its tags in two CI files is caught: %s" % found)
    # GitLab's hosted runners beside the project's: the tree's classes hold, and each of
    # their rules broken in a copy - in .gitlab-ci.yml or in an included file, one that
    # FILES names through its key - is caught (runner_problems, runner_class_problems).
    found = runner_class_problems(texts)
    if found:
        failures.append("the tree's classes of jobs for GitLab's runners hold: %s" % found)
    files_key = dict((relative.replace(os.sep, "/"), key) for key, relative in FILES.items())

    def both(copy):
        return runner_problems(copy)[0] + runner_class_problems(copy)

    def class_broken(what, path, old, new, needle, judge=both):
        copy = dict(texts)
        key = "ci" if path == ".gitlab-ci.yml" else files_key.get(path)
        source = copy.get(key) if key else copy["ci-includes"].get(path)
        if not source or old not in source:
            failures.append("%s: the text to break is not in %s (%r)" % (what, path, old))
            return
        changed = source.replace(old, new, 1)
        if key:
            copy[key] = changed
        else:
            copy["ci-includes"] = dict(copy["ci-includes"], **{path: changed})
        found = judge(copy)
        if not any(needle in p for p in found):
            failures.append("%s is caught: %s" % (what, found))

    hosted_file = HOSTED_CHECKS_FILE
    sbom_job = "linux-sbom:\n  extends: .linix\n"
    for what, path, old, new, needle in (
            ("a job that takes a class's switch its class does not name", ".gitlab/ci/snap.yml", "\nsnap-build-arm64:\n",
             "\nprobe-classed:\n  extends: .snap-build\n  needs: []\n\nsnap-build-arm64:\n",
             "probe-classed (.gitlab/ci/snap.yml) takes its tags from HM_RUNNER_PACKAGES, and that class does not name it"),
            ("a job its class names taken out of it", ".gitlab/ci/packages-appimage.yml",
             "appimage-test-x86_64:\n  extends: [.appimage, .appimage-follow-rules]\n",
             "appimage-test-x86_64:\n  extends: [.appimage, .appimage-follow-rules]\n  tags: [linix]\n",
             "appimage-test-x86_64 is one of HM_RUNNER_PACKAGES's jobs (RUNNER_CLASSES), and it does not take its tags"),
            ("a job on a switch beside another tag", ".gitlab-ci.yml", sbom_job, sbom_job + "  tags: [$HM_RUNNER_BUILD, linix]\n",
             "or a class's switch alone"),
            ("a switch whose default is GitLab's runner", ".gitlab-ci.yml", "  HM_RUNNER_PACKAGES: linix\n",
             "  HM_RUNNER_PACKAGES: saas-linux-large-amd64\n", "set HM_RUNNER_PACKAGES to 'saas-linux-large-amd64': it must be linix"),
            ("a lane's default changed", ".gitlab-ci.yml", "  HM_LANE_SUITE: linix-suite\n", "  HM_LANE_SUITE: hosted-$CI_JOB_NAME\n",
             "set HM_LANE_SUITE to 'hosted-$CI_JOB_NAME': it must be linix-suite"),
            ("a switch set nowhere", ".gitlab-ci.yml", "  HM_RUNNER_ARM64: linix\n", "", "do not set HM_RUNNER_ARM64"),
            ("a job that sends itself to GitLab's runners", ".gitlab/ci/source-tarball.yml",
             "  variables:\n    DEBIAN_FRONTEND: noninteractive\n",
             "  variables:\n    DEBIAN_FRONTEND: noninteractive\n    HM_RUNNER_BUILD: saas-linux-large-amd64\n",
             ".gitlab/ci/source-tarball.yml sets HM_RUNNER_BUILD in source-tarball"),
            ("a workflow rule that sets a switch", ".gitlab-ci.yml",
             '    - if: $CI_COMMIT_BRANCH && $CI_COMMIT_REF_PROTECTED == "true"\n    - if: $CI_COMMIT_BRANCH\n',
             '    - if: $CI_COMMIT_BRANCH && $CI_COMMIT_REF_PROTECTED == "true"\n      variables:\n'
             '        HM_RUNNER_SUITE: saas-linux-large-amd64\n    - if: $CI_COMMIT_BRANCH\n', ".gitlab-ci.yml sets HM_RUNNER_SUITE in workflow"),
            ("a classed job that names an environment", ".gitlab-ci.yml", sbom_job,
             sbom_job + "  environment:\n    name: probe\n    action: access\n", "linux-sbom, one of HM_RUNNER_BUILD's jobs, says environment:"),
            ("a classed job given an ID token", ".gitlab-ci.yml", sbom_job,
             sbom_job + "  id_tokens:\n    PROBE_ID_TOKEN:\n      aud: probe\n", "linux-sbom, one of HM_RUNNER_BUILD's jobs, says id_tokens:"),
            ("a classed job's image GitLab's runners would not pull", ".gitlab-ci.yml",
             "    pull_policy: [if-not-present, always]\n  before_script:\n    - python3 build/check-ci-image.py --job\n",
             "    pull_policy: if-not-present\n  before_script:\n    - python3 build/check-ci-image.py --job\n",
             "linux-build, one of HM_RUNNER_BUILD's jobs, asks for the pull policy if-not-present"),
            ("a classed job's service GitLab's runners would not pull", ".gitlab-ci.yml",
             "      alias: mariadb\n      pull_policy: [if-not-present, always]\n", "      alias: mariadb\n      pull_policy: if-not-present\n",
             "linux-db-upgrade-mysql, one of HM_RUNNER_BUILD's jobs, asks for the pull policy if-not-present"),
            ("an arm64 job in the CI image", ".gitlab/ci/packages-deb.yml",
             "deb-ubuntu26.04-arm64-install:\n  extends: [.deb-install-arm64, .deb-image-ubuntu26.04-arm64]\n",
             "deb-ubuntu26.04-arm64-install:\n  extends: [.deb-image-ubuntu26.04-arm64, .linix, .deb-install-arm64]\n",
             "deb-ubuntu26.04-arm64-install, one of HM_RUNNER_ARM64's jobs, runs in the CI image"),
            ("an amd64 class's job in an arm64 container", ".gitlab/ci/packages-deb.yml",
             "deb-debian12-amd64-install:\n  extends: [.deb-install, .deb-image-debian12-amd64]\n",
             "deb-debian12-amd64-install:\n  extends: [.deb-install, .deb-image-debian12-arm64]\n",
             "deb-debian12-amd64-install, one of HM_RUNNER_PACKAGES's jobs, runs an arm64 container"),
            ("a laned job out of its lane", ".gitlab/ci/alpine.yml", "  resource_group: $HM_LANE_SUITE\n", "  resource_group: linix-suite\n",
             "alpine-suite, one of HM_RUNNER_SUITE's jobs, waits in the resource group linix-suite"),
            ("a job of a class without a lane in a resource group", ".gitlab-ci.yml", sbom_job, sbom_job + "  resource_group: probe\n",
             "linux-sbom, one of HM_RUNNER_BUILD's jobs, waits in the resource group probe: its class has no lane"),
            ("a job outside the class in its lane", ".gitlab/ci/nix.yml", "  resource_group: linix-package-build\n",
             "  resource_group: $HM_LANE_PACKAGES\n", "nix waits in $HM_LANE_PACKAGES, HM_RUNNER_PACKAGES's lane"),
            ("the emulated aarch64 builds out of their lane on linix", ".gitlab/ci/packages-rpm.yml",
             "  tags: [linix]\n  resource_group: linix-package-build\n", "  tags: [linix]\n",
             "rpm-el10-aarch64 waits in $HM_LANE_PACKAGES, HM_RUNNER_PACKAGES's lane"),
            ("nix without KVM", ".gitlab/ci/nix.yml", "  tags: [linix, kvm]\n", "  tags: [linix]\n", "nix boots a QEMU machine"),
            ("the checks not included", ".gitlab-ci.yml", "  - local: .gitlab/ci/hosted-runners.yml\n", "",
             ".gitlab-ci.yml does not include .gitlab/ci/hosted-runners.yml"),
            ("a switch the checks do not ask of", hosted_file, ' && $HM_RUNNER_SUITE == "linix"', "",
             ".hosted-switched (.gitlab/ci/hosted-runners.yml) does not ask whether HM_RUNNER_SUITE is linix"),
            ("a lane the checks do not ask of", hosted_file, ' && $HM_LANE_PACKAGES == "linix-package-build"', "",
             "does not ask whether HM_LANE_PACKAGES is linix-package-build"),
            ("the checks' rules without .own-runners'", hosted_file, "    - !reference [.own-runners, rules]\n", "    - when: on_success\n",
             "does not end with .own-runners' rules"),
            ("a check off linix", hosted_file, "hosted-runners:\n  stage: check\n  tags: [linix]\n",
             "hosted-runners:\n  stage: check\n  tags: [$HM_RUNNER_BUILD]\n",
             "hosted-runners (.gitlab/ci/hosted-runners.yml) does not run on linix alone"),
            ("compute-quota out of its token's environment", hosted_file, "    name: compute-quota\n", "    name: quota\n",
             "compute-quota (.gitlab/ci/hosted-runners.yml) does not name the environment compute-quota"),
            ("hosted-runners given an environment", hosted_file, "  script:\n    - python3 build/ci/hosted-runners.py --self-test\n",
             "  environment:\n    name: compute-quota\n  script:\n    - python3 build/ci/hosted-runners.py --self-test\n",
             "hosted-runners (.gitlab/ci/hosted-runners.yml) names an environment"),
            ("a check that runs another command", hosted_file, "    - python3 build/ci/hosted-runners.py quota\n",
             "    - python3 build/ci/hosted-runners.py --self-test\n",
             "compute-quota (.gitlab/ci/hosted-runners.yml) does not run python3 build/ci/hosted-runners.py quota"),
            ("compute-quota allowed to fail by some answers alone", hosted_file,
             "  allow_failure: true\n  script:\n    - python3 build/ci/hosted-runners.py quota\n",
             "  allow_failure:\n    exit_codes: [2, 3]\n  script:\n    - python3 build/ci/hosted-runners.py quota\n",
             "compute-quota (.gitlab/ci/hosted-runners.yml) does not say allow_failure: true"),
            ("an ID token every job is given, by default:", ".gitlab-ci.yml", "  tags: [$HM_RUNNER]\n",
             "  tags: [$HM_RUNNER]\n  id_tokens:\n    PROBE_ID_TOKEN:\n      aud: probe\n",
             ".gitlab-ci.yml's default: says id_tokens:"),
            ("an ID token every job is given, by default: in flow style", ".gitlab-ci.yml", "  tags: [$HM_RUNNER]\n",
             "  tags: [$HM_RUNNER]\n  'id_tokens': {PROBE_ID_TOKEN: {aud: probe}}\n", ".gitlab-ci.yml's default: says id_tokens:"),
            ("default: taking keys by a merge key", ".gitlab-ci.yml", "  tags: [$HM_RUNNER]\n",
             "  tags: [$HM_RUNNER]\n  <<: *probe-keys\n", ".gitlab-ci.yml's default: takes keys by a merge key or an alias"),
            ("a GitLab template whose variables hosted-runners does not know", ".gitlab/ci/gitlab-security.yml",
             "  - template: Jobs/Secret-Detection.gitlab-ci.yml\n",
             "  - template: Jobs/Secret-Detection.gitlab-ci.yml\n  - template: Jobs/Dependency-Scanning.gitlab-ci.yml\n",
             "the CI files include GitLab's template Jobs/Dependency-Scanning.gitlab-ci.yml, and build/ci/hosted-runners.py's "
             "TEMPLATE_GLOBALS does not name its variables"),
            ("a GitLab template no longer included, its variables still known", ".gitlab/ci/gitlab-security.yml",
             "  - template: Jobs/Secret-Detection.gitlab-ci.yml\n", "",
             "build/ci/hosted-runners.py's TEMPLATE_GLOBALS names Jobs/Secret-Detection.gitlab-ci.yml, which no CI file "
             "includes"),
            ("a classed job a merge request's pipeline makes", ".gitlab-ci.yml", sbom_job,
             sbom_job + '  rules:\n    - if: $CI_PIPELINE_SOURCE == "merge_request_event"\n',
             "linux-sbom (.gitlab-ci.yml) is tagged ['linix'], for a runner of the project's, and its rule 1")):
        class_broken(what, path, old, new, needle)
    held_runners = PROJECT_RUNNERS
    try:
        globals()["PROJECT_RUNNERS"] = tuple((who, tuple(t for t in carried if t != "kvm")) for who, carried in held_runners)
        found = runner_class_problems(texts)
        if not any("no runner of the project's carries ['linix', 'kvm']" in p for p in found):
            failures.append("kvm carried by no runner of the project's is caught: %s" % found)
    finally:
        globals()["PROJECT_RUNNERS"] = held_runners

    class Script(object):
        CLASSES = (("HM_RUNNER_BUILD", "amd64", None, None), ("HM_RUNNER_PACKAGES", "amd64", "HM_LANE_PACKAGES", "linix-package-build"),
                   ("HM_RUNNER_ARM64", "arm64", None, None), ("HM_RUNNER_SUITE", "amd64", "HM_LANE_SUITE", "linix-suite"))
        HOSTED = dict(HOSTED_MACHINES)
        TEMPLATE_GLOBALS = {"Jobs/SAST.gitlab-ci.yml": ("SAST_EXCLUDED_PATHS",),
                            "Jobs/Secret-Detection.gitlab-ci.yml": ("SECRETS_ANALYZER_VERSION",)}
    if runner_class_problems(texts, Script):
        failures.append("hosted-runners.py's classes as the CI files have them pass: %s" % runner_class_problems(texts, Script))
    held_templates = Script.TEMPLATE_GLOBALS
    Script.TEMPLATE_GLOBALS = {"Jobs/SAST.gitlab-ci.yml": ("SAST_EXCLUDED_PATHS",)}
    if not any("template Jobs/Secret-Detection.gitlab-ci.yml, and build/ci/hosted-runners.py's TEMPLATE_GLOBALS does not "
               "name its variables" in p for p in runner_class_problems(texts, Script)):
        failures.append("hosted-runners.py not knowing an included template's variables is caught")
    Script.TEMPLATE_GLOBALS = held_templates
    Script.CLASSES = Script.CLASSES[:3]
    if not any("CLASSES are not RUNNER_CLASSES'" in p for p in runner_class_problems(texts, Script)):
        failures.append("hosted-runners.py without the suites' class is caught")
    Script.CLASSES, Script.HOSTED = Script.CLASSES + (("HM_RUNNER_SUITE", "amd64", "HM_LANE_SUITE", "linix-suite"),), \
        dict(HOSTED_MACHINES, arm64=("saas-linux-small-arm64",))
    if not any("HOSTED machines are not HOSTED_MACHINES" in p for p in runner_class_problems(texts, Script)):
        failures.append("hosted-runners.py naming other machines is caught")
    # Each Python script of GATE_FILES keeps its own directory off sys.path before it
    # imports anything but os and sys.
    sources = gate_python_sources(landing)
    if path_guard_problems(sources) or len(sources) < 9:
        failures.append("the gates' Python scripts keep their directory off sys.path (%d read): %s" % (
            len(sources), path_guard_problems(sources)))
    guarded = (sources.get("build/ci/security-gate.py") or "").replace("\r\n", "\n")
    for what, text in (("taken away", guarded.replace(PATH_GUARD, "pass", 1)),
                       ("after an import", guarded.replace("import os\nimport sys\n", "import os\nimport sys\nimport json\n", 1)),
                       ("off another directory", guarded.replace("os.path.dirname(os.path.realpath(__file__))",
                                                                 "os.path.dirname(os.path.realpath(sys.argv[0]))", 1))):
        if text == guarded or not path_guard_problems({"build/ci/security-gate.py": text}):
            failures.append("the sys.path guard %s is caught" % what)
    # Its documents_only, as a landing runs it, against the CI files' not-docs-only.
    landing = landing_module()
    held = getattr(landing, LANDING_DOCUMENTS, None)
    for what, stand_in, needle in (
            ("every Markdown file a document to land-via-mr", lambda paths: all(p.endswith(".md") for p in paths),
             "(documents_only says True; the CI files scan it)"),
            ("no batch documents alone to land-via-mr", lambda paths: False,
             "(documents_only says False; the CI files do not scan it)"),
            ("a document under any dot-directory code to land-via-mr",
             lambda paths: all(p.endswith(".md") and ("/" not in p or p.startswith(".gitlab/")) for p in paths),
             ".github/a/b.md (documents_only says False"),
            ("land-via-mr without documents_only", None, "has no documents_only")):
        try:
            if stand_in is None:
                delattr(landing, LANDING_DOCUMENTS)
            else:
                setattr(landing, LANDING_DOCUMENTS, stand_in)
            found, _ = check(texts, chain, feed)
        finally:
            setattr(landing, LANDING_DOCUMENTS, held)
        if not any(needle in p for p in found):
            failures.append("%s is caught: %s" % (what, found))
    # One malformed entry after the entries the file holds, if any: the list from its
    # key to the end of the file is what each case replaces.
    listed = re.search(r"^exceptions:.*", texts["exceptions"], re.MULTILINE | re.DOTALL)
    listed = listed.group(0) if listed else "exceptions: []\n"
    kept = "exceptions:\n" if listed.startswith("exceptions: []") else listed.rstrip("\n") + "\n"
    malformed = ("  - scan: semgrep\n    finding: 0123456789abcdef0123\n    what: x\n"
                 "    reason: 'read, and the input is a literal the code itself writes'\n    reviewed-by: Progressiverobot\n"
                 "    reviewed: 2026-09-26\n")
    broken("an exception without an expiry", "exceptions", listed, kept + malformed, "it has no expires")
    broken("an exception for two years", "exceptions", listed, kept + malformed + "    expires: 2028-09-26\n",
           "reviewed again within a year")
    broken("the exceptions file given to everyone", "codeowners", "/.gitlab/security-exceptions.yml", "/.gitlab/security-exceptions.old",
           "does not give /.gitlab/security-exceptions.yml to @Progressiverobot")
    broken("the secret scan's ignore list shared", "codeowners", "/.gitleaksignore                   @Progressiverobot",
           "/.gitleaksignore                   @Progressiverobot @zainulabidin1990", "does not give /.gitleaksignore")
    broken("secret_detection with GitLab's allowlist again", "security", SECRET_RULES_STEP, "    - /analyzer run\n",
           "secret_detection does not put the rules build/ci/secret-detection-rules.sh derives")
    broken("the rules made and not put where the analyzer reads them", "security", "    - cp /tmp/hm-gitleaks.toml /gitleaks.toml\n",
           "", "secret_detection does not put the rules build/ci/secret-detection-rules.sh derives")
    broken("the rules made with a table of the tree's", "security", " /gitleaks.toml /tmp/hm-gitleaks.toml\n",
           " /gitleaks.toml .gitlab/secret-detection-allowlist.toml /tmp/hm-gitleaks.toml\n",
           "secret_detection does not put the rules build/ci/secret-detection-rules.sh derives")
    broken("signing in a child pipeline", "ci", "  script:\n    - sh build/ci/release-sign.sh", "  trigger:\n    include: child.yml\n  script:\n    - sh build/ci/release-sign.sh", "release-sign is a trigger")

    # The files 6.3.6 adds: gathered with their builds' tests, none escaping on a tag.
    broken("release not waiting for the gathered files", "ci", "    - job: release-packages\n      artifacts: true\n", "",
           "release does not need release-packages")
    broken("release without the gathered files", "ci", "    - job: release-packages\n      artifacts: true\n",
           "    - job: release-packages\n      artifacts: false\n", "release does not bring release-packages's artifacts")
    broken("release needing the chart optionally", "ci", "    - job: helm-chart-package\n      artifacts: true\n",
           "    - job: helm-chart-package\n      artifacts: true\n      optional: true\n", "release needs helm-chart-package optionally")
    broken("release without the chart", "ci", "    - job: helm-chart-package\n      artifacts: true\n", "", "release does not need helm-chart-package")
    broken("the gathering file not included", "ci", "  - local: .gitlab/ci/release-packages.yml\n", "", "does not include .gitlab/ci/release-packages.yml")
    broken("a distribution release's build not waited for", "release-packages", "    - job: rpm-el9-aarch64\n      artifacts: true\n", "",
           "release-packages does not need rpm-el9-aarch64, which makes")
    broken("an install job not waited for", "release-packages", "    - job: deb-debian12-arm64-install\n      artifacts: false\n", "",
           "release-packages does not need deb-debian12-arm64-install")
    broken("an AppImage's test not waited for", "release-packages", "    - job: appimage-test-aarch64\n      artifacts: false\n", "",
           "release-packages does not need appimage-test-aarch64")
    broken("the tarball not gathered", "release-packages", "    - job: source-tarball\n      artifacts: true\n", "",
           "release-packages does not need source-tarball")
    broken("a build gathered without its artifacts", "release-packages", "    - job: rpm-leap16-x86_64\n      artifacts: true\n",
           "    - job: rpm-leap16-x86_64\n      artifacts: false\n", "must need rpm-leap16-x86_64 with artifacts: true")
    broken("a build needed optionally", "release-packages", "    - job: deb-ubuntu24.04-amd64\n      artifacts: true\n",
           "    - job: deb-ubuntu24.04-amd64\n      artifacts: true\n      optional: true\n", "needs deb-ubuntu24.04-amd64 optionally")
    broken("a job gathered that makes nothing gathered", "release-packages", "    - job: source-tarball\n      artifacts: true\n",
           "    - job: source-tarball\n      artifacts: true\n    - job: linux-build\n      artifacts: true\n", "needs linux-build, which makes nothing")
    broken("the gathering job allowed to fail", "release-packages", "  timeout: 30 minutes\n", "  timeout: 30 minutes\n  allow_failure: true\n",
           "release-packages can let release start")
    broken("the gathering job not made for a tag", "release-packages", "    - if: $CI_COMMIT_TAG !~ /^v/ && $CI_COMMIT_BRANCH !~ /^batch/\n",
           "    - if: $CI_COMMIT_BRANCH !~ /^batch/\n", "is not made for a v* tag and a batch* branch alone")
    broken("the gathering job off linix", "release-packages", "  extends: .linix\n", "  image: python:3\n",
           "does not run build/ci/release-chain.py gather on linix")
    broken("an AppImage left for a press on a tag", "packages-appimage",
           '    - if: $CI_COMMIT_TAG !~ /^v/ && $CI_COMMIT_BRANCH !~ /^batch/ && $CI_COMMIT_REF_PROTECTED == "true"\n      when: manual\n',
           '    - if: $CI_COMMIT_BRANCH !~ /^batch/ && $CI_COMMIT_REF_PROTECTED == "true"\n      when: manual\n',
           "appimage-runtime-x86_64 can let release start in a v* tag's pipeline")
    broken("an AppImage left for a press on a batch branch", "packages-appimage",
           '    - if: $CI_COMMIT_TAG !~ /^v/ && $CI_COMMIT_BRANCH !~ /^batch/ && $CI_COMMIT_REF_PROTECTED == "true"\n      when: manual\n',
           '    - if: $CI_COMMIT_TAG !~ /^v/ && $CI_COMMIT_REF_PROTECTED == "true"\n      when: manual\n',
           "waits for a press on a batch* branch")
    broken("a distribution release's install test allowed to fail", "packages-rpm", ".rpm-install:\n  stage: test\n",
           ".rpm-install:\n  stage: test\n  allow_failure: true\n", "rpm-el10-x86_64-install can let release start")
    broken("an AppImage test run without its image", "packages-appimage", "  needs: [appimage-x86_64]\n", "  needs: []\n",
           "appimage-test-x86_64 does not need appimage-x86_64")
    broken("the chart not packaged for a tag", "helm", '($CI_COMMIT_TAG =~ /^v/ || $CI_COMMIT_BRANCH =~ /^batch/)',
           '($CI_COMMIT_BRANCH =~ /^batch/)', "does not package the chart into helm-chart/ for a v* tag")
    broken("the chart allowed to fail", "helm", "  timeout: 10 minutes\n", "  timeout: 10 minutes\n  allow_failure: true\n",
           "helm-chart-package can let release start")
    many = "".join("    - job: extra-%d\n      artifacts: false\n" % i for i in range(20))
    broken("a job needing more than GitLab takes", "ci", "    - job: semgrep-sast\n      artifacts: true\n",
           "    - job: semgrep-sast\n      artifacts: true\n" + many, "GitLab.com takes 50")
    # The copies and the provenance.
    broken("the bundles not copied to .sigstore.json", "sign", '      cp "$bundle" "$file.sigstore.json"\n', "", "copied to <file>.sigstore.json")
    broken("the provenance not attested", "sign", 'cosign attest-blob $attest_flags --statement "$sboms/$statement" --bundle "$out/$provenance"',
           'true', "the statement attested as the pipeline")
    broken("the files not found in the provenance", "sign", "cosign verify-blob-attestation $verify_flags", "true", "every file found in the provenance")
    broken("the provenance to another log", "sign", 'attest_flags="--yes --new-bundle-format --rekor-url https://rekor.sigstore.dev',
           'attest_flags="--yes --new-bundle-format --rekor-url https://log2025-1.rekor.sigstore.dev', "attests the provenance without")
    broken("the provenance under another name", "sign", 'provenance="hmailserver-${version}.intoto.sigstore.json"',
           'provenance="hmailserver-${version}.intoto.jsonl"', "the provenance's name")
    broken("a copy signed as a file of the release", "sign", "  case \"$file\" in *.cosign.bundle|*.sigstore.json) continue ;; esac",
           "  case \"$file\" in *.cosign.bundle) continue ;; esac", "or no longer passes over them")
    broken("the AppImages not held to their SBOMs", "verify", ' "$signed"/*.AppImage', "", "does not hold hMailServer-9.9.9-x86_64.AppImage to its SBOM")
    broken("the SBOM count left at 5", "verify", '[ "$described" -eq 27 ]', '[ "$described" -eq 5 ]', "holds 5 file(s) to their SBOM")
    broken("the provenance not checked", "verify", "python3 build/ci/release-chain.py check-provenance", "true", "check-provenance")
    broken("the release's provenance not checked", "verify",
           '  python3 build/ci/release-chain.py check-provenance --assets "$signed" \\\n    || fail "the .sigstore.json copies or the '
           'provenance are not what the release is"', '  true', "check-provenance")
    broken("a rehearsal's provenance checked as a release's", "verify",
           'check-provenance --assets "$signed" --rehearse', 'check-provenance --assets "$signed"', "check-provenance")
    broken("a rehearsal bundle the server accepts", "verify", '[ "$code" -ne 0 ] || fail', 'true || fail', "no longer requires the server's verifier to refuse")
    broken("publish uploading before the provenance is held", "chain", "provenance_problems(\n        assets, version,",
           "list(\n        assets, version,", "held the release to provenance_problems(")
    broken("publish uploading without the budget", "chain", "    unfit = check_budget(api, tag, version, digests,", "    unfit = list(api, tag, version, digests,",
           "held the release to check_budget")
    broken("a feed limit that is not the server's", "checker", "const size_t MAX_FEED_BYTES = 4 * 1024 * 1024;",
           "const size_t MAX_FEED_BYTES = 2 * 1024 * 1024;", "a server reads 2097152")
    # The rehearsal.
    broken("the rehearsal not included", "ci", "  - local: .gitlab/ci/release-rehearsal.yml\n", "", "does not include .gitlab/ci/release-rehearsal.yml")
    broken("a rehearsal on master", "rehearsal", " || $CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH\n", "\n", "is not exactly: never for")
    broken("a rehearsal on a tag", "rehearsal", " || $CI_COMMIT_TAG || ", " || ", "is not exactly: never for")
    broken("a rehearsal job with rules of its own", "rehearsal", "  rules:\n    - !reference [.release-rehearsal-rules, rules]\n  id_tokens:",
           "  rules:\n    - if: $CI_COMMIT_BRANCH\n  id_tokens:", "release-rehearsal-sign's rules are not .release-rehearsal-rules alone")
    broken("a rehearsal that publishes", "rehearsal", "    - sh build/ci/release-sign.sh --rehearse\n",
           "    - sh build/ci/release-sign.sh --rehearse\n    - python3 build/ci/release-chain.py publish\n", "names publish")
    broken("a rehearsal with the job token", "rehearsal", "    COSIGN_YES: \"true\"\n", "    COSIGN_YES: \"true\"\n    TOKEN: $CI_JOB_TOKEN\n",
           "names CI_JOB_TOKEN")
    broken("a rehearsal signing as a release", "rehearsal", "    - sh build/ci/release-sign.sh --rehearse\n", "    - sh build/ci/release-sign.sh\n",
           "release-rehearsal-sign runs")
    broken("a rehearsal verifier holding a token", "rehearsal", "  timeout: 15 minutes\n",
           "  timeout: 15 minutes\n  id_tokens:\n    SIGSTORE_ID_TOKEN:\n      aud: sigstore\n", "release-rehearsal-verify holds an ID token")
    broken("a rehearsal signing with another token", "rehearsal", "      aud: sigstore\n", "      aud: sigstore\n    AZURE_ID_TOKEN:\n      aud: api://AzureADTokenExchange\n",
           "or another token besides")
    broken("a rehearsal without the branch's installer", "rehearsal", "    - job: windows-packages-x64\n      artifacts: true\n", "",
           "release-rehearsal does not need windows-packages-x64")
    broken("a rehearsal without the branch's ARM64 installer", "rehearsal", "    - job: windows-packages-arm64\n      artifacts: true\n", "",
           "release-rehearsal does not need windows-packages-arm64")
    broken("a rehearsal without the ARM64 component list", "rehearsal", " sbom/native-dependencies-arm64.json sbom/native-dependencies-x64.json",
           " sbom/native-dependencies-x64.json",
           "release-rehearsal does not keep linux-sbom's ARM64 component list")
    # The ARM64 installer in a release.
    broken("release without the ARM64 installer's job", "ci",
           "    - job: windows-packages-arm64\n      artifacts: true\n", "", "release does not need windows-packages-arm64")
    broken("release without the ARM64 installer's artifacts", "ci",
           "    - job: windows-packages-arm64\n      artifacts: true\n", "    - job: windows-packages-arm64\n      artifacts: false\n",
           "release does not bring windows-packages-arm64's artifacts")
    broken("release needing the ARM64 installer's job optionally", "ci",
           "    - job: windows-packages-arm64\n      artifacts: true\n", "    - job: windows-packages-arm64\n      artifacts: true\n      optional: true\n",
           "release needs windows-packages-arm64 optionally")
    broken("the ARM64 installer's job allowed to fail", "packages-windows",
           "      artifacts: false\n      optional: true\n  variables:\n    HM_PACKAGE_PLATFORM: ARM64",
           "      artifacts: false\n      optional: true\n  allow_failure: true\n  variables:\n    HM_PACKAGE_PLATFORM: ARM64",
           "windows-packages-arm64 can let release start in a v* tag's pipeline without its having passed")
    broken("the ARM64 installer's job allowed to fail with an exit code", "packages-windows",
           "      artifacts: false\n      optional: true\n  variables:\n    HM_PACKAGE_PLATFORM: ARM64",
           "      artifacts: false\n      optional: true\n  allow_failure:\n    exit_codes: [3]\n  variables:\n    HM_PACKAGE_PLATFORM: ARM64",
           "windows-packages-arm64 can let release start in a v* tag's pipeline without its having passed "
           "(windows-packages-arm64 says allow_failure: exit_codes: [3])")
    broken("the ARM64 installer's template allowed to fail with an exit code, in flow style", "packages-windows",
           "\n.windows-packages:\n  extends: .own-runners\n",
           "\n.windows-packages:\n  extends: .own-runners\n  allow_failure: {exit_codes: [2]}\n",
           "windows-packages-arm64 can let release start in a v* tag's pipeline without its having passed "
           "(.windows-packages says allow_failure: {exit_codes: [2]})")
    broken("the ARM64 installer's job gone", "packages-windows", "\nwindows-packages-arm64:\n", "\nwindows-packages-arm65:\n",
           "has no job windows-packages-arm64")
    broken("a bench150 job without the leftovers' hook", "ci",
           "  tags: [bench150]\n  hooks: !reference [.bench150-leftovers, hooks]\n  needs:\n    - job: release\n",
           "  tags: [bench150]\n  needs:\n    - job: release\n",
           "release-authenticode in .gitlab-ci.yml runs on bench150 without hooks")
    # The same in an included file, which the rule reads as the CI file includes it.
    windows_file = ".gitlab/ci/packages-windows.yml"
    # The template's own lines: build-inputs-authenticode, before it in the file, takes
    # the hook too.
    template_head = ".windows-packages:\n  extends: .own-runners\n  stage: release\n  tags: [bench150]\n"
    hooked = template_head + "  hooks: !reference [.bench150-leftovers, hooks]\n"
    if hooked not in texts["ci-includes"].get(windows_file, ""):
        failures.append("a bench150 template without the leftovers' hook: the text to break is not in %s" % windows_file)
    else:
        copy = dict(texts, **{"ci-includes": dict(texts["ci-includes"], **{
            windows_file: texts["ci-includes"][windows_file].replace(hooked, template_head, 1)})})
        found, _ = check(copy, chain, feed)
        if not any(".windows-packages in .gitlab/ci/packages-windows.yml runs on bench150 without hooks" in p for p in found):
            failures.append("a bench150 template without the leftovers' hook is caught: %s" % found)
    broken("the branch's x64 installer built for a tag too", "packages-windows",
           "  rules:\n    - if: $CI_COMMIT_TAG\n      when: never\n    - !reference [.own-runners, rules]\n", "",
           "windows-packages-x64 does not refuse a v* tag first")
    broken("the ARM64 build needing the branch's x64 build on a tag", "packages-windows",
           "    - job: windows-packages-x64\n      artifacts: false\n      optional: true\n",
           "    - job: windows-packages-x64\n      artifacts: false\n",
           "windows-packages-arm64 needs windows-packages-x64 without optional: true")
    broken("release-authenticode going on to the ARM64 installer when the x64 one was not signed", "ci",
           "-OutDir release-authenticode\n      if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n", "-OutDir release-authenticode\n",
           "release-authenticode does not stop when the x64 installer's call fails")
    broken("the ARM64 installer not signed", "ci",
           '      if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n      & (Get-Process -Id $PID).Path -NoProfile -ExecutionPolicy Bypass -File build\\ci\\windows-authenticode.ps1 -Installer "release-assets\\hMailServer-$version-arm64.exe" -OutDir release-authenticode-arm64\n',
           "", "release-authenticode does not sign the ARM64 installer")
    broken("the signed ARM64 installer not kept", "ci", "      - release-authenticode-arm64/\n", "",
           "release-authenticode does not sign the ARM64 installer")
    broken("the ARM64 component list not kept for release-sbom", "ci", " sbom/native-dependencies-arm64.json sbom/native-dependencies-x64.json",
           " sbom/native-dependencies-x64.json",
           "release does not keep linux-sbom's ARM64 component list")
    broken("release-sign.sh taking the x64 installer alone", "sign", 'for installer in "$installer_x64" "$installer_arm64"; do',
           'for installer in "$installer_x64"; do', "both installers taken")
    # The PowerShell module, signed by its own call after the installers.
    broken("release-authenticode going on to the module when the ARM64 installer was not signed", "ci",
           "-OutDir release-authenticode-arm64\n      if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n", "-OutDir release-authenticode-arm64\n",
           "release-authenticode does not stop when the ARM64 installer's call fails")
    broken("the module not signed", "ci",
           "      & (Get-Process -Id $PID).Path -NoProfile -ExecutionPolicy Bypass -File build\\ci\\windows-authenticode.ps1 -Module release-assets -OutDir release-authenticode-module\n",
           "", "release-authenticode does not sign the PowerShell module")
    broken("the signed module not kept", "ci", "      - release-authenticode-module/\n", "", "release-authenticode does not sign the PowerShell module")
    broken("release-sign.sh taking the module unsigned, from collect", "sign",
           '      HmailServer.psd1|HmailServer.psm1) echo "$authenticode-module/$1" ;;\n', "", "the module taken signed")
    broken("windows-authenticode.ps1 signing the module without holding it to its own bytes", "authenticode",
           "$problems += @(Get-BlockProblems ", "$problems += @(Write-Output ", "(Get-BlockProblems)")
    held_module = chain.MODULE_FILES
    try:
        chain.MODULE_FILES = ()
        found, _ = check(texts, chain, feed)
        if not any("does not carry the PowerShell module's" in p for p in found):
            failures.append("a release that no longer carries the module is caught: %s" % found)
    finally:
        chain.MODULE_FILES = held_module
    # The build inputs' signatures, checked on bench150 before any build uses them.
    inputs_head = "build-inputs-authenticode:\n  extends: .own-runners\n  stage: build\n  tags: [bench150]\n  hooks: !reference [.bench150-leftovers, hooks]\n  needs: []\n"
    broken("the build inputs' job gone", "packages-windows", "\nbuild-inputs-authenticode:\n", "\nbuild-inputs-authenticode-2:\n",
           "has no job build-inputs-authenticode")
    broken("the build inputs' job allowed to fail", "packages-windows", inputs_head, inputs_head + "  allow_failure: true\n",
           "build-inputs-authenticode can let a build start without its having passed")
    broken("the build inputs' job waiting for a press", "packages-windows", inputs_head, inputs_head + "  when: manual\n",
           "build-inputs-authenticode can let a build start without its having passed")
    broken("the build inputs' job on another runner", "packages-windows", inputs_head, inputs_head.replace("[bench150]", "[linix]"),
           "build-inputs-authenticode does not run on bench150")
    broken("the build inputs' job without .own-runners' rules", "packages-windows", inputs_head,
           inputs_head.replace("  extends: .own-runners\n", ""), "build-inputs-authenticode does not run on bench150 under .own-runners")
    broken("the build inputs' job going on to the self-test after its check failed", "packages-windows",
           "build\\ci\\build-inputs-authenticode.ps1\n      if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n",
           "build\\ci\\build-inputs-authenticode.ps1\n", "does not stop when the build inputs' check fails")
    broken("the build inputs' job without its self-test", "packages-windows",
           "      & $shell -NoProfile -ExecutionPolicy Bypass -File build\\ci\\authenticode-selftest.ps1\n", "",
           "build-inputs-authenticode does not run build\\ci\\authenticode-selftest.ps1")
    broken("the Windows packages built without the build inputs' job", "packages-windows",
           "    - job: build-inputs-authenticode\n      artifacts: false\n", "", ".windows-packages does not need build-inputs-authenticode")
    broken("the Windows packages needing the build inputs' job optionally", "packages-windows",
           "    - job: build-inputs-authenticode\n      artifacts: false\n",
           "    - job: build-inputs-authenticode\n      artifacts: false\n      optional: true\n", ".windows-packages does not need build-inputs-authenticode")
    broken("windows-installer built without the build inputs' job", "ci", "needs: [windows-gate, build-inputs-authenticode]",
           "needs: [windows-gate]", "windows-installer does not need build-inputs-authenticode")
    broken("release-sign.sh taking the ARM64 installer unsigned", "sign", '      *-arm64.exe) echo "$authenticode-arm64/$1" ;;\n', "",
           "the ARM64 installer taken signed")
    broken("a job of the rehearsal's file that is not the rehearsal's", "rehearsal", "\nrelease-rehearsal-verify:\n",
           "\nrelease-rehearsal-publish:\n  script: [true]\n\nrelease-rehearsal-verify:\n", "is not the rehearsal's")
    broken("--rehearse taken with publish", "chain", 'if args.rehearse and args.command not in ("collect", "sbom", "check-provenance"):',
           'if args.rehearse and args.command not in ("collect", "sbom", "check-provenance", "publish"):', "takes --rehearse with a command")
    broken("a rehearsal's collect given the job's token", "chain",
           'problems = collect(ROOT, None, os.path.join(ROOT, args.out or "release-assets"), os.environ, api=None, rehearse=True)',
           'problems = collect(ROOT, None, os.path.join(ROOT, args.out or "release-assets"), os.environ, api=GitLab(token), rehearse=True)',
           "gives a rehearsal's collect the job's token")
    held_gathered = chain.GATHERED
    try:
        chain.GATHERED = "packages"
        found, _ = check(texts, chain, feed)
        if not any("collects the gathered files from packages/" in p for p in found):
            failures.append("gathered files collected from elsewhere are caught: %s" % found)
    finally:
        chain.GATHERED = held_gathered
    held_builds = chain.gathered_builds
    try:
        chain.gathered_builds = lambda version: held_builds(version) + [dict(held_builds(version)[0], name="x.deb", job="deb-debian11-amd64",
                                                                               test="deb-debian11-amd64-install")]
        found, _ = check(texts, chain, feed)
        if not any("no package file has the job deb-debian11-amd64" in p for p in found):
            failures.append("a gathered file no job makes is caught: %s" % found)
    finally:
        chain.gathered_builds = held_builds

    # The source of the AppImages' runtime's libraries: missing from the release, missing
    # from its sums, gathered from elsewhere or with an SBOM, or its job not wired.
    sources_file = "hmailserver-9.9.9-appimage-runtime-sources.tar.gz"
    for what, replacement, needle in (
            ("the AppImages released without their runtime's sources",
             lambda version: [b for b in held_builds(version) if b["kind"] != "runtime-sources"],
             "the release carries the AppImages without %s" % sources_file),
            ("the runtime's sources gathered from another job's artifact",
             lambda version: [dict(b, path="appimage/" + b["name"]) if b["kind"] == "runtime-sources" else b for b in held_builds(version)],
             "gathers %s from appimage/" % sources_file),
            ("the runtime's sources held to no sums",
             lambda version: [dict(b, sums=None) if b["kind"] == "runtime-sources" else b for b in held_builds(version)],
             "held to None")):
        chain.gathered_builds = replacement
        try:
            found, _ = check(texts, chain, feed)
        finally:
            chain.gathered_builds = held_builds
        if not any(needle in p for p in found):
            failures.append("%s is caught: %s" % (what, found))
    held_members = chain.sums_members
    chain.sums_members = lambda version: [n for n in held_members(version) if not n.endswith("-appimage-runtime-sources.tar.gz")]
    try:
        found, _ = check(texts, chain, feed)
    finally:
        chain.sums_members = held_members
    if not any("%s with no line in the release's sums" % sources_file in p for p in found):
        failures.append("the runtime's sources without a line in the release's sums is caught: %s" % found)
    held_compiled_assets = chain.compiled_assets
    chain.compiled_assets = lambda version: held_compiled_assets(version) + [chain.runtime_sources_name(version)]
    try:
        found, _ = check(texts, chain, feed)
    finally:
        chain.compiled_assets = held_compiled_assets
    if not any("counts %s a compiled file" % sources_file in p for p in found):
        failures.append("the runtime's sources counted a compiled file is caught: %s" % found)
    broken("the runtime's sources not waited for", "release-packages", "    - job: appimage-runtime-sources\n      artifacts: true\n", "",
           "release-packages does not need appimage-runtime-sources")
    broken("the runtime's sources gathered without their artifacts", "release-packages",
           "    - job: appimage-runtime-sources\n      artifacts: true\n", "    - job: appimage-runtime-sources\n      artifacts: false\n",
           "must need appimage-runtime-sources with artifacts: true")
    broken("an AppImage built without its runtime's licence files", "packages-appimage",
           "  needs: [appimage-runtime-x86_64, appimage-runtime-sources]\n", "  needs: [appimage-runtime-x86_64]\n",
           "appimage-x86_64 does not need appimage-runtime-sources")
    broken("the runtime's sources packed from one runtime alone", "packages-appimage",
           "  needs: [appimage-runtime-x86_64, appimage-runtime-aarch64]\n", "  needs: [appimage-runtime-x86_64]\n",
           "appimage-runtime-sources does not need appimage-runtime-aarch64")
    broken("the runtime's sources job not packing", "packages-appimage", "    - sh build/ci/packages/appimage-runtime-sources.sh pack ",
           "    - sh build/ci/packages/appimage-runtime-sources.sh record ", "has no appimage-runtime-sources job that runs")
    broken("the runtime's sources job made where a tag may pass it", "packages-appimage",
           "appimage-runtime-sources:\n  extends: [.appimage, .appimage-follow-rules, .no-github]\n",
           "appimage-runtime-sources:\n  extends: [.appimage, .appimage-follow-rules, .no-github]\n  allow_failure: true\n",
           "appimage-runtime-sources can let release start in a v* tag's pipeline")

    # The source of the libraries the AppImages carry in usr/lib: missing from the
    # release, missing from its sums, gathered from elsewhere, held to no sums or with
    # an SBOM, its job not wired, or a place naming another file than the release's.
    bundled_file = "hmailserver-9.9.9-appimage-bundled-sources.tar.gz"
    for what, replacement, needle in (
            ("the AppImages released without their bundled libraries' sources",
             lambda version: [b for b in held_builds(version) if b["kind"] != "bundled-sources"],
             "the release carries the AppImages without %s" % bundled_file),
            ("the bundled libraries' sources gathered from another job's artifact",
             lambda version: [dict(b, path="appimage/" + b["name"]) if b["kind"] == "bundled-sources" else b for b in held_builds(version)],
             "gathers %s from appimage/" % bundled_file),
            ("the bundled libraries' sources made by another job",
             lambda version: [dict(b, job="appimage-x86_64") if b["kind"] == "bundled-sources" else b for b in held_builds(version)],
             "made by appimage-x86_64"),
            ("the bundled libraries' sources held to no sums",
             lambda version: [dict(b, sums=None) if b["kind"] == "bundled-sources" else b for b in held_builds(version)],
             "held to None")):
        chain.gathered_builds = replacement
        try:
            found, _ = check(texts, chain, feed)
        finally:
            chain.gathered_builds = held_builds
        if not any(needle in p for p in found):
            failures.append("%s is caught: %s" % (what, found))
    chain.sums_members = lambda version: [n for n in held_members(version) if not n.endswith("-appimage-bundled-sources.tar.gz")]
    try:
        found, _ = check(texts, chain, feed)
    finally:
        chain.sums_members = held_members
    if not any("%s with no line in the release's sums" % bundled_file in p for p in found):
        failures.append("the bundled libraries' sources without a line in the release's sums is caught: %s" % found)
    chain.compiled_assets = lambda version: held_compiled_assets(version) + [chain.bundled_sources_name(version)]
    try:
        found, _ = check(texts, chain, feed)
    finally:
        chain.compiled_assets = held_compiled_assets
    if not any("counts %s a compiled file" % bundled_file in p for p in found):
        failures.append("the bundled libraries' sources counted a compiled file is caught: %s" % found)
    broken("the bundled libraries' sources not waited for", "release-packages",
           "    - job: appimage-bundled-sources\n      artifacts: true\n", "",
           "release-packages does not need appimage-bundled-sources")
    broken("the bundled libraries' sources gathered without their artifacts", "release-packages",
           "    - job: appimage-bundled-sources\n      artifacts: true\n", "    - job: appimage-bundled-sources\n      artifacts: false\n",
           "must need appimage-bundled-sources with artifacts: true")
    broken("the bundled libraries' sources packed from one AppImage alone", "packages-appimage",
           "  needs: [appimage-x86_64, appimage-aarch64]\n", "  needs: [appimage-x86_64]\n",
           "appimage-bundled-sources does not pack from appimage-aarch64's record")
    broken("the bundled libraries' sources packed without the aarch64 record", "packages-appimage",
           "pack appimage/built-against-x86_64 appimage/built-against-aarch64\n",
           "pack appimage/built-against-x86_64\n", "appimage-bundled-sources does not pack from appimage-aarch64's record")
    broken("the bundled libraries' sources job not packing", "packages-appimage",
           "    - bash build/ci/packages/appimage-bundled-sources.sh pack ", "    - bash build/ci/packages/appimage-bundled-sources.sh check ",
           "has no appimage-bundled-sources job that runs")
    broken("the bundled libraries' sources job keeping nothing", "packages-appimage",
           "    paths:\n      - appimage-bundled-sources/\n", "    paths:\n      - appimage-bundled-sources/x\n",
           "has no appimage-bundled-sources job that runs")
    broken("the bundled libraries' sources job made where a tag may pass it", "packages-appimage",
           "appimage-bundled-sources:\n  extends: [.appimage, .appimage-follow-rules, .no-github]\n",
           "appimage-bundled-sources:\n  extends: [.appimage, .appimage-follow-rules, .no-github]\n  allow_failure: true\n",
           "appimage-bundled-sources can let release start in a v* tag's pipeline")
    # .no-github in the two sources jobs.
    runtime_extends = "appimage-runtime-sources:\n  extends: [.appimage, .appimage-follow-rules, .no-github]\n"
    bundled_extends = "appimage-bundled-sources:\n  extends: [.appimage, .appimage-follow-rules, .no-github]\n"
    broken("the runtime's sources job without .no-github", "packages-appimage", runtime_extends,
           "appimage-runtime-sources:\n  extends: [.appimage, .appimage-follow-rules]\n",
           "appimage-runtime-sources in .gitlab")
    broken("the bundled libraries' sources job without .no-github", "packages-appimage", bundled_extends,
           "appimage-bundled-sources:\n  extends: [.appimage, .appimage-follow-rules]\n",
           "appimage-bundled-sources in .gitlab")
    broken("the bundled libraries' sources job with a before_script of its own", "packages-appimage", bundled_extends,
           bundled_extends + "  before_script:\n    - apt-get update\n", "appimage-bundled-sources in .gitlab")
    broken("the runtime's sources job with a one-line before_script of its own", "packages-appimage", runtime_extends,
           runtime_extends + "  before_script: [apk add curl]\n", "appimage-runtime-sources in .gitlab")
    # .no-github named ahead of a template that has a before_script: the later one
    # decides the key.
    copy = dict(texts)
    copy["packages-appimage"] = copy["packages-appimage"].replace(
        runtime_extends, "appimage-runtime-sources:\n  extends: [.appimage, .no-github, .appimage-before]\n", 1) + \
        "\n.appimage-before:\n  before_script:\n    - apk add curl\n"
    found, _ = check(copy, chain, feed)
    if runtime_extends not in texts["packages-appimage"] or not any("appimage-runtime-sources in .gitlab" in p for p in found):
        failures.append("the runtime's sources job's .no-github ahead of a template with a before_script is caught: %s" % found)
    broken(".no-github gone from its file", "no-github", "\n.no-github:\n", "\n.no-github-gone:\n",
           "appimage-bundled-sources in .gitlab")
    for what, old, new in (
            ("the runtime's sources job with its own before_script that begins with .no-github's", runtime_extends,
             "appimage-runtime-sources:\n  extends: [.appimage, .appimage-follow-rules]\n  before_script:\n"
             "    - !reference [.no-github, before_script]\n    - apk add curl\n"),
            ("the runtime's sources job with .no-github behind a template with no before_script", runtime_extends,
             "appimage-runtime-sources:\n  extends: [.no-github, .appimage, .appimage-follow-rules]\n")):
        copy = dict(texts)
        copy["packages-appimage"] = copy["packages-appimage"].replace(old, new, 1)
        found, _ = check(copy, chain, feed)
        if old not in texts["packages-appimage"] or any("does not keep GitHub out of itself" in p for p in found):
            failures.append("%s passes: %s" % (what, [p for p in found if "GitHub" in p]))
    broken("the image naming another file than the release's", "appimage-pack",
           'BUNDLED_SOURCES="hmailserver-${VERSION}-appimage-bundled-sources.tar.gz"',
           'BUNDLED_SOURCES="hmailserver-${VERSION}-appimage-sources.tar.gz"',
           "the image's bundled.txt) does not name hmailserver-${VERSION}-appimage-bundled-sources.tar.gz")
    broken("the image's test holding it to another file", "appimage-test",
           '"sources hmailserver-$expected-appimage-bundled-sources.tar.gz"', '"sources hmailserver-$expected-appimage-sources.tar.gz"',
           "the image's test) does not name")
    broken("the pack making another file than the release's", "bundled-sources",
           'name="hmailserver-$version-appimage-bundled-sources.tar.gz"', 'name="hmailserver-$version-sources.tar.gz"',
           "the pack) does not name")
    broken("the image without hMailServer's AGPL-3.0 text", "appimage-pack",
           '"${APPDIR}/usr/share/hmailserver/licenses/AGPL-3.0.txt"\nchmod 0644 "${APPDIR}/usr/share/hmailserver/licenses/AGPL-3.0.txt"',
           '"${APPDIR}/usr/share/hmailserver/licenses/LICENSE"\nchmod 0644 "${APPDIR}/usr/share/hmailserver/licenses/LICENSE"',
           "no longer puts hMailServer's own AGPL-3.0 text in the image")

    # The release tag and the registry.
    owner_line = [l for l in texts["signers"].split("\n") if l.strip() and not l.startswith("#")]
    if owner_line:
        broken("a second key in the allow-list", "signers", owner_line[0], owner_line[0] + "\nsomeone@example.org ssh-ed25519 " + _key(7),
               "lists 2 key(s)")
    broken("publish uploading before it looks at the registry", "chain", "    if conflicts:\n        raise ReleaseError",
           "    if False:\n        raise ReleaseError", "before it uploads anything")
    broken("publish sending again what is up already", "chain", "        if name in same:\n", "        if False:\n", "sends again a file")
    broken("publish holding the newest record alone", "chain", "        for entry in entries:\n            if (entry.get(\"file_sha256\")",
           "        for entry in entries[-1:]:\n            if (entry.get(\"file_sha256\")", "every record of each name")
    broken("collect not looking at the registry", "chain", "problems.extend(check_registry_before_signing(api, version,",
           "problems.extend(check_nothing(api, version,", "collect no longer looks at the registry")
    broken("collect without the job's token", "chain", "api=GitLab(token) if token else None", "api=None", "the job's token")

    def patched(what, attribute, value, needle):
        held = getattr(chain, attribute)
        setattr(chain, attribute, value)
        try:
            found, _ = check(texts, chain, feed)
        finally:
            setattr(chain, attribute, held)
        if not any(needle in p for p in found):
            failures.append("%s is caught: %s" % (what, found))

    def tree_allow_list(root, tag, commit, run):
        """The tag check as it was: the tagged tree's own allow-list."""
        verified = run(["git", "-C", root, "-c", "gpg.ssh.allowedSignersFile=.gitlab/allowed_signers", "verify-tag", tag])
        return [] if verified.returncode == 0 else ["not signed"]

    held_check = chain.check_tag

    def without(needle):
        return lambda root, tag, commit, run: [p for p in held_check(root, tag, commit, run) if needle not in p]

    patched("the allow-list read from the tagged tree", "check_tag", tree_allow_list, "the tagged tree's own allow-list")
    patched("a tag check that asks no ancestry", "check_tag", without("not on master"), "accepts a tag on a commit that is not on master")
    patched("a tag check that takes a list of two keys", "check_tag", without("key(s)"), "a second key master's list holds")
    patched("a tag check that takes git's word for the key", "check_tag", without("not by the one key"), "a key other than the one git was given")
    patched("a tag check that refuses everything", "check_tag", lambda root, tag, commit, run: ["no"], "refuses a tag master's one key signed")
    patched("the allow-list read from another branch", "TRUSTED_BRANCH", "main", "not from master")
    held_sources = chain.sources
    patched("the aarch64 .rpm collected from where no job leaves it", "sources",
            lambda version: dict(held_sources(version), **{"hmailserver-%s-1.aarch64.rpm" % version:
                                                           os.path.join("packages-rpm", "fedora44-aarch64", "hmailserver-%s-1.aarch64.rpm" % version)}),
            "collects hmailserver-9.9.9-1.aarch64.rpm from")
    held_compiled = chain.compiled_assets
    patched("the arm64 .deb not counted a compiled file", "compiled_assets",
            lambda version: [n for n in held_compiled(version) if not n.endswith("_arm64.deb")], "compiled_assets does not carry hmailserver_9.9.9_arm64.deb")
    held_expected = chain.expected_assets
    patched("a package carried without its SBOM", "expected_assets",
            lambda version: [n for n in held_expected(version) if not n.endswith(".deb.cdx.json")], "without its SBOM")
    patched("a compiled file the SBOM rule does not know", "expected_assets",
            lambda version: held_expected(version) + ["hMailServer-%s-arm32.exe" % version], "each of them needs its SBOM")
    patched("collect taking whatever the registry holds", "check_registry_before_signing", lambda api, version, digests: [], "collect takes a registry holding")
    held_registry = chain.check_registry_before_signing
    patched("collect taking an installer already signed", "check_registry_before_signing",
            lambda api, version, digests: [p for p in held_registry(api, version, digests) if "earlier run's signing" not in p],
            "holding the installer")
    patched("collect refusing the same bytes", "check_registry_before_signing",
            lambda api, version, digests: held_registry(api, version, dict((n, "f" * 64) for n in digests)), "collect refuses a registry holding the Linux files")

    # The certificate readers: the tree's pass, and each reads a planted certificate
    # wrongly once it searches the bytes.
    module = certificate_module()
    found, notes = certificate_problems(module)
    if found:
        failures.append("the certificate readers pass: %s" % found)
    if any("no sh here" in n for n in notes):
        print("NOTE: %s; the shell reader's negative control is skipped" % notes[0])

    def searching(certificate):
        """The reader release-verify.sh had: the OID's first place in the bytes."""
        at = certificate.find(module.REPOSITORY_ID_OID)
        if at < 0:
            return "absent"
        rest = certificate[at + len(module.REPOSITORY_ID_OID):]
        rest = rest[3:] if rest[:1] == b"\x01" else rest
        if rest[:1] != b"\x04" or rest[2:3] != b"\x0c":
            return "refused"
        return "id=" + rest[4:4 + rest[3]].decode("utf-8", "replace")
    found, _ = certificate_problems(module, shell=False, answer_of=searching)
    if not any("planted" in p for p in found):
        failures.append("a Python reader that searches the certificate's bytes is caught: %s" % found)
    if not any("no sh here" in n for n in notes):
        work = tempfile.mkdtemp(prefix="hm-check-certificate-")
        try:
            searcher = os.path.join(work, "searching.sh")
            with open(searcher, "w", encoding="utf-8", newline="\n") as f:
                # The reader container-image-deb.sh had: the OID's first place in the hex.
                f.write("set -eu\n"
                        "raw=$(grep -o '\"rawBytes\": \"[^\"]*\"' \"$2\" | head -n 1 | cut -d'\"' -f4)\n"
                        "hex=$(printf '%%s' \"$raw\" | base64 -d | od -An -tx1 -v | tr -d ' \\n')\n"
                        "case \"$hex\" in\n"
                        "  *%s*) rest=\"${hex#*%s}\"; rest=\"${rest#0101ff}\"\n"
                        "    case \"$rest\" in\n"
                        "      040a0c08%s*) echo id=%s ;;\n"
                        "      *) echo 'refused: another id'; exit 1 ;;\n"
                        "    esac ;;\n"
                        "  *) echo absent ;;\n"
                        "esac\n" % (module.REPOSITORY_ID_OID.hex(), module.REPOSITORY_ID_OID.hex(),
                                     module.PROJECT_ID.encode().hex(), module.PROJECT_ID))
            found, _ = certificate_problems(module, script=searcher)
            if not any("sigstore-certificate.sh reads" in p and "planted" in p for p in found):
                failures.append("a shell reader that searches the certificate's bytes is caught: %s" % found)
        finally:
            shutil.rmtree(work, ignore_errors=True)
    empty = tempfile.mkdtemp(prefix="hm-check-no-sh-")
    try:
        found, _ = certificate_problems(module, environ={"CI": "true", "PATH": empty})
    finally:
        shutil.rmtree(empty, ignore_errors=True)
    if not any("no sh" in p for p in found):
        failures.append("a CI job without sh, where the shell reader is not held to the Python one, is caught: %s" % found)
    scanned = scanned_texts()
    if byte_search_problems(scanned):
        failures.append("no file searches a certificate's bytes: %s" % byte_search_problems(scanned))
    for what, text in (("as hex", 'oid=%s' % module.REPOSITORY_ID_OID.hex()),
                       ("as a Python list", "oid = bytes([%s])" % ", ".join("0x%02X" % b for b in module.REPOSITORY_ID_OID)),
                       ("as escapes", 'oid = b"%s"' % "".join("\\x%02x" % b for b in module.REPOSITORY_ID_OID)),
                       ("as decimal od output", 'case "$bytes" in *"%s"*) ;; esac' % "   ".join(
                           str(b) for b in module.REPOSITORY_ID_OID)),
                       ("as a decimal Python list", "oid = bytes([%s])" % ", ".join(str(b) for b in module.REPOSITORY_ID_OID)),
                       ("as the base64 of a certificate that holds it", 'case "$raw" in *%s*) ;; esac' % base64.b64encode(
                           module.REAL_CERTIFICATE).decode()[module.REAL_CERTIFICATE.find(module.REPOSITORY_ID_OID) * 4 // 3 - 4:][:24]),
                       ("as base64, a byte after a place base64 starts", 'grep -c %s' % base64.b64encode(
                           b"\x01" + module.REPOSITORY_ID_OID).decode()[2:14]),
                       ("  ... two bytes after", 'grep -c %s' % base64.b64encode(
                           b"\x01\x02" + module.REPOSITORY_ID_OID).decode()[3:16])):
        planted = dict(scanned, **{"build/ci/release-verify.sh": scanned["build/ci/release-verify.sh"] + "\n" + text + "\n"})
        if not any("release-verify.sh holds the Sigstore OID arc" in p for p in byte_search_problems(planted)):
            failures.append("the OID's bytes %s in release-verify.sh are caught" % what)
    broken("release-verify.sh reading the id by itself", "verify",
           'said=$(python3 build/ci/sigstore-certificate.py repository-id "$signed/$installer.cosign.bundle")',
           'said=$(python3 - "$signed/$installer.cosign.bundle" < /dev/null)', "release-verify.sh no longer has said=")
    broken("release-verify.sh taking any id", "verify", "  id=86730559) id_row=", "  id=*) id_row=", "the id this project's")
    broken("container-image-deb.sh reading the id by itself", "image-deb",
           'said=$(sh build/ci/sigstore-certificate.sh repository-id "$out/$name.cosign.bundle")',
           'said=$(od -An -tx1 "$out/$name.cosign.bundle")', "the project's id read from the certificate's extensions")
    broken("container-image-deb.sh taking any id", "image-deb", '"id=$project_id") id_said="$project_id" ;;',
           'id=*) id_said="$project_id" ;;', "the project's id in the certificate")
    broken("container-image-deb.sh verifying a bundle of the old format", "image-deb", 'verify_flags="--new-bundle-format"',
           'verify_flags=""', "the bundle read strictly")
    broken("container-image-deb.sh going on with a cosign that has no --new-bundle-format", "image-deb",
           "  || fail \"this cosign has no verify-blob --new-bundle-format", "  || true \"this cosign has no verify-blob "
           "--new-bundle-format", "never the old format's cert")

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def run_checks(texts, chain, feed):
    """check()'s (problems, notes), with each file of FILES the tree lacks named first: a
    problem, never a traceback. Where a missing file leaves check() unable to go on, what
    it raised is a problem too; with every file there, whatever it raises is a fault of
    this checker, and is raised."""
    problems = ["%s is not in the tree: this checker reads it, so the rules that hold it cannot be checked" % name
                for name in texts.get("missing", [])]
    try:
        found, notes = check(texts, chain, feed)
    except Exception as error:
        if not problems:
            raise
        return problems + ["the rest was not checked without it: %s: %s" % (type(error).__name__, error)], []
    return problems + found, notes


def main():
    feed = _load(os.path.join("build", "generate-update-feed.py"), "generate_update_feed")
    chain = _load(os.path.join("build", "ci", "release-chain.py"), "release_chain")
    if "--self-test" in sys.argv[1:]:
        return self_test(chain, feed)
    problems, notes = run_checks(load_texts(), chain, feed)
    found, said = certificate_problems(certificate_module())
    problems += found + byte_search_problems(scanned_texts())
    notes += said
    for note in notes:
        print("NOTE: " + note)
    for problem in problems:
        print("PROBLEM: " + problem)
    if problems:
        print("check-release-chain: %d problem(s)" % len(problems))
        return 1
    print("check-release-chain: the release jobs are what a server built after 6.3.3 needs (%d jobs)." % len(RELEASE_JOBS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
