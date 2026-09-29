#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The release jobs of .gitlab-ci.yml are what a server built after 6.3.3 needs them to be.

A v* tag's pipeline ends in the release chain (docs/ContinuousIntegration.md, "The
release"): windows-installer, release, release-authenticode, release-sbom,
release-sign, release-verify, release-publish and update-feed. Most of what makes a release
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
  * the identity the scripts expect is not the one SigstoreVerifier.cpp trusts,
    or that one is not this project's path, file and repository id; or the
    verifier stops trusting the group the project is to move to (MOVE_TO) once,
    with the project's id required, or stops requiring that id of every
    certificate naming the group's path or repository whatever the settings; or
    the scripts sign from a path it does not trust, or name the identity twice;
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
  * update-feed is not a trigger of this project's master that passes no
    variables, or a job other than pages would run in the pipeline it starts: that
    pipeline's source is "pipeline", and every job refuses it (and a schedule),
    except the few whose own rules can only match something else, each named here
    with why. Its pipeline is told apart by its source because a trigger job's
    variables reach the downstream pipeline as pipeline variables, which GitLab
    refuses by default in a project made since 17.7;
  * a scheduled pipeline is not chosen by its schedule's description: linux-fuzz,
    linux-coverage, renovate, forge-export (.gitlab/ci/forge-export.yml) and secret_detection
    (.gitlab/ci/gitlab-security.yml, the whole history) each run for the schedule
    described "fuzz", "coverage", "renovate", "export" or "security", and pages
    refuses those five, and package-repos (.gitlab/ci/package-repos.yml) takes pages' rules and no
    others; or a rule of those files still names
    HM_PIPELINE or RUN_RENOVATE, the variables that chose them before, which the
    project would refuse on a schedule - one way in;
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
    its report after the analyzer, which exits 0 whatever it finds;
  * the release can be collected past the security scans: release must need
    static-analysis, semgrep, dependency-scan and GitLab's semgrep-sast, not
    optionally, with their artifacts - the scans build/ci/security-gate.py reads -
    semgrep-sast must run for a protected v* tag and keep gl-sast-report.json as a
    file whatever it found, and collect must ask the gate before it copies anything;
    or .gitlab/security-exceptions.yml is not a file the gate reads, or accepts a
    finding without a reason, a reviewer or an expiry within a year of the review, or
    is not the owner's in .gitlab/CODEOWNERS (nor .gitleaksignore, the secret scan's
    list of what is not a secret).
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
    release's files are signed with, or no longer attests the image's SBOM
    (container-image-sbom, build/ci/container-image-sbom.sh) to it and verifies the
    attestation; or container-image-start does not start the pushed image with the
    command its test script names, on a network of its own; or the switch, the
    project's own CI/CD variable HM_PUBLISH_IMAGE, is not read as the jobs run -
    container-image-deb.sh and linux-image.sh must refuse, exiting non-zero, unless
    it is "1" - or is read by a rule, which is decided when the pipeline is made and
    would leave a release published before the project's move without the jobs; or
    a committed CI file sets that variable;
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
    licence files each AppImage job needs; or a Linux file the release carries, the
    chart or the runtime's sources has no line in the release's sums;
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

Usage:
  python build/check-release-chain.py              the tree as it is
  python build/check-release-chain.py --self-test  each rule broken in a copy, and caught
"""

import base64
import importlib.util
import io
import json
import os
import re
import subprocess
import shutil
import sys
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
    "image-start": os.path.join("build", "ci", "container-image-start-test.sh"),
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
}

RELEASE_JOBS = ["windows-installer", "release", "release-authenticode", "release-sbom", "release-sign", "release-verify",
                "release-publish", "update-feed"]
# The container image's jobs, in .gitlab/ci/container-image.yml, and what each must
# need: the .deb is taken from the release once it is published, the image built from
# it and pushed, and signed, scanned and started once it is pushed.
IMAGE_JOBS = {
    "container-image-deb": ["release-publish"],
    "container-image": ["container-image-deb"],
    "container-image-sbom": ["container-image", "container-image-deb"],
    "container-image-sign": ["container-image", "container-image-sbom"],
    "container_scanning": ["container-image"],
    "container-image-start": ["container-image"],
}
# The first of them, which a maintainer starts by hand; every other one needs it, and
# so runs only after it.
IMAGE_FIRST = "container-image-deb"
# The switch: the project's own CI/CD variable HM_PUBLISH_IMAGE, read as the jobs RUN.
# The maintainers set it after the project's move into the group hmailserver, which
# they decided on 27 September 2026 to make before the first image, so that the first
# image's address, registry.gitlab.com/hmailserver/hmailserver, is the lasting one (a
# container repository's path is always its project's). The jobs are in every v* tag's
# pipeline, so that a release published before the move has its image pushed from its
# own pipeline afterwards; a rule reading the variable would be decided when the
# pipeline is made, and would leave that release without them. The two scripts that
# fetch and push refuse, exiting non-zero, unless it is "1". Nothing committed may set it.
RUNTIME_GATE = 'if [ "${HM_PUBLISH_IMAGE:-}" != 1 ]; then'
GATED_SCRIPTS = ("image-deb", "image-build")
# Jobs that need not begin with REFUSAL, because their own rules match nothing the
# feed's pipeline (source "pipeline", on master) is - each with the text that makes
# it so. None of them is a gate of a tag's pipeline, so release need not wait for
# them either.
EXEMPT = {
    "pages": ("$CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH", "the feed pipeline's one job"),
    "linux-fuzz": ('$CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "fuzz" &&', "only the fuzz schedule"),
    "linux-coverage": ('$CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "coverage" &&', "only the coverage schedule"),
    "renovate": ('$CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "renovate" &&', "only Renovate's schedule"),
    "sign-off": ('$CI_COMMIT_BRANCH =~ /^batch/ && $CI_COMMIT_REF_PROTECTED == "true"', "only a merge request or a batch* branch"),
    "installer-smoke": ("($CI_COMMIT_TAG =~ /^v/ || $CI_COMMIT_BRANCH =~ /^batch/)", "only a v* tag or a batch* branch"),
}
TAG_ONLY = '$CI_COMMIT_TAG =~ /^v/ && $CI_COMMIT_REF_PROTECTED == "true"'
# What every job but EXEMPT's begins its rules with, when: never: every scheduled
# pipeline, and the feed's downstream pipeline, whose CI_PIPELINE_SOURCE is "pipeline".
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
# The job that gathers every other file a release carries from the package jobs
# (.gitlab/ci/release-packages.yml): each distribution release's .deb and .rpm, the
# AppImages and the source tarball, each with its SBOM; and the chart's job.
GATHER_JOB = "release-packages"
CHART_JOB = "helm-chart-package"
# The job that makes the source of the libraries linked into the AppImages' runtime,
# and where it leaves it (.gitlab/ci/packages-appimage.yml).
RUNTIME_SOURCES_JOB = "appimage-runtime-sources"
RUNTIME_SOURCES_DIR = "appimage-runtime-sources/"
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
                           "rpm-fedora44-aarch64-sbom", GATHER_JOB, CHART_JOB, "windows-packages-x64"],
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
COLLECTED = ["linux-build", "linux-sbom", "windows-installer"] + [
    job[role] for _, job in sorted(CARRIED_PACKAGES.items()) for role in ("build", "sbom")] + [GATHER_JOB, CHART_JOB]
# The security scans whose reports release reads (build/ci/security-gate.py's SCANS):
# needed, not optionally, with their artifacts. semgrep-sast is GitLab's, in
# .gitlab/ci/gitlab-security.yml; the others are the CI file's own, on linix.
SCANNED = ["static-analysis", "semgrep", "dependency-scan", "semgrep-sast"]
# The files a change to which is the owner's alone to accept.
OWNED = ["/.gitlab/security-exceptions.yml", "/.gitleaksignore"]
COSIGN_IMAGE = re.compile(r'^\s*HM_COSIGN_IMAGE:\s*"?(\S+?)"?\s*$', re.MULTILINE)
PINNED_COSIGN = re.compile(r"^gcr\.io/projectsigstore/cosign:[A-Za-z0-9._\-]+@sha256:[0-9a-f]{64}$")
PLACEHOLDER_COSIGN = re.compile(r"^gcr\.io/projectsigstore/cosign:[A-Za-z0-9._\-]+@sha256:PIN-THE-DIGEST$")
RESERVED = {"stages", "workflow", "default", "variables", "include"}
# Where the project is to move, from the personal namespace to the group the owner
# reserved for it (26 September 2026). SigstoreVerifier.cpp trusts the pipeline there
# too, with the project's id required, so that the servers built before the move take
# what is signed after it; the release scripts sign from either path and no other.
MOVE_TO = "https://gitlab.com/hmailserver/hmailserver"
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


def tag_escapes(name, jobs):
    """How a job, in a v* tag's pipeline, could let a job that needs it start without its
    having passed: allow_failure: true or when: manual in the job or a template it
    extends, or in a rule of the rules it takes that a v* tag can match. A rule whose
    if: asks for $CI_COMMIT_TAG !~ /^v/, with && alone, is one no tag matches. jobs
    holds every block the job, its templates and their references can name."""
    found = []
    order = template_order(name, jobs)
    for current in order:
        for line in jobs[current].split("\n"):
            if re.match(r"^  (allow_failure:\s*true|when:\s*manual)\s*$", line):
                found.append("%s says %s" % (current, line.strip()))
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
         Registry(*[(n, "a" * 64) for n in digests if n != names["windows"]]), False),
        ("a .deb with other bytes", Registry((names["deb-amd64"], "b" * 64)), True),
        ("a .deb held twice, the older with other bytes", Registry((names["deb-amd64"], "b" * 64), (names["deb-amd64"], "a" * 64)), True),
        ("the installer, which Authenticode makes after collect", Registry((names["windows"], "a" * 64)), True),
        ("the installer's SBOM, which release-sbom makes after collect", Registry((chain.sbom_name(names["windows"]), "a" * 64)), True),
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
                        "a schedule and the feed's pipeline (%s)" % REFUSAL)
    for name in RELEASE_JOBS:
        text = rules_text(name, jobs)
        if TAG_ONLY not in text:
            problems.append("%s can run for something that is not a protected v* tag: its rules must be .release-rules" % name)
        if re.search(r"^  trigger:", jobs[name], re.MULTILINE) and name != "update-feed":
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
        "release": gates(jobs) + ["windows-installer"] + carried + [GATHER_JOB, CHART_JOB],
        "release-authenticode": ["release"],
        "release-sbom": ["release", "release-authenticode"],
        "release-sign": ["release", "release-authenticode", "release-sbom"],
        "release-verify": ["release", "release-sign"],
        "release-publish": ["release-sign", "release-verify"],
        "update-feed": ["release-publish"],
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
    if re.search(r"^  id_tokens:", sbom_job, re.MULTILINE) or re.search(r"^  tags:", sbom_job, re.MULTILINE):
        problems.append("release-sbom holds an ID token or runs on the project's own runners: it signs nothing and needs neither")
    if "sbom/native-dependencies-x64.json release-tools/" not in jobs["release"]:
        problems.append("release does not keep linux-sbom's component list in release-tools/, which release-sbom reads")
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
    path = feed.PROJECT_URL[len("https://"):]
    expected_prefix = "https://%s//.gitlab-ci.yml@refs/tags/v" % path
    if not prefix or prefix.group(1) != expected_prefix:
        problems.append("SigstoreVerifier.cpp's GITLAB_IDENTITY_PREFIX is %s, not %s - this project's path and this CI file" % (
            prefix.group(1) if prefix else "missing", expected_prefix))
    if chain.SIGNER_IDENTITY_PREFIX + "v" != expected_prefix:
        problems.append("build/ci/release-chain.py expects the identity %s..., not the verifier's %s" % (chain.SIGNER_IDENTITY_PREFIX, expected_prefix))
    # The group the project is to move to: the same pipeline under its new path, the
    # same project's id, and that id required - were it optional, a certificate for the
    # path would be accepted from whoever held the path.
    group_prefix = re.search(r'const char \*GITLAB_GROUP_IDENTITY_PREFIX = "([^"]+)";', texts["verifier"])
    group_repository = re.search(r'const char \*GITLAB_GROUP_REPOSITORY = "([^"]+)";', texts["verifier"])
    expected_group_prefix = MOVE_TO + IDENTITY_TAIL + "v"
    if not group_prefix or group_prefix.group(1) != expected_group_prefix:
        problems.append("SigstoreVerifier.cpp's GITLAB_GROUP_IDENTITY_PREFIX is %s, not %s - the path the project is to move to and this CI file" % (
            group_prefix.group(1) if group_prefix else "missing", expected_group_prefix))
    if not group_repository or group_repository.group(1) != MOVE_TO:
        problems.append("SigstoreVerifier.cpp's GITLAB_GROUP_REPOSITORY is not %s" % MOVE_TO)
    if not re.search(r'trust\.signers\.push_back\(MakeSigner_\(GITLAB_IDENTITY_PREFIX, GITLAB_ISSUER, GITLAB_REPOSITORY, GITLAB_REPOSITORY_ID, (true|false), 0, ""\)\);',
                     texts["verifier"]):
        problems.append("SigstoreVerifier.cpp's DefaultTrust no longer trusts the GitLab signer at the project's present path with its id")
    group_signer = 'trust.signers.push_back(MakeSigner_(GITLAB_GROUP_IDENTITY_PREFIX, GITLAB_ISSUER, GITLAB_GROUP_REPOSITORY, GITLAB_REPOSITORY_ID, false, 0, ""));'
    if group_signer not in texts["verifier"] or texts["verifier"].count("MakeSigner_(GITLAB_GROUP_IDENTITY_PREFIX") != 1:
        problems.append("SigstoreVerifier.cpp's DefaultTrust no longer trusts the group's signer once, with the project's id required: %s" % group_signer)
    # And whatever the settings make of the signers, a certificate naming the group's
    # path or repository needs the project's id: the check in Verify_ that every
    # signer's match goes through.
    for needle in ('LowerAscii_(std::string(GITLAB_GROUP_REPOSITORY) + "//")',
                   'LowerAscii_(repository) == LowerAscii_(GITLAB_GROUP_REPOSITORY)',
                   'repositoryId != std::string(GITLAB_REPOSITORY_ID)',
                   "         else if (!groupRefusal.IsEmpty())\n            why = groupRefusal;\n"):
        if needle not in texts["verifier"]:
            problems.append("SigstoreVerifier.cpp no longer requires the project's id of every certificate naming the group's path "
                            "or repository, whatever the settings (%s)" % needle.strip())
    # The scripts name the identity after the project's path, which must be one of the
    # two the verifier trusts, each with its own repository, and nothing else: the case
    # on CI_PROJECT_PATH exactly as below, the one assignment of the repository and the
    # identity in each script, and the identity right after the case.
    paths = [(feed.PROJECT_URL.split("/", 3)[3], feed.PROJECT_URL), (MOVE_TO.split("/", 3)[3], MOVE_TO)]
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
    if not repository or repository.group(1) != feed.PROJECT_URL:
        problems.append("SigstoreVerifier.cpp's GITLAB_REPOSITORY is not %s" % feed.PROJECT_URL)
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
    build = texts["image-build"]
    if "\n    registry.gitlab.com/hmailserver/hmailserver) ;;\n" not in build or \
            re.search(r"^[ \t]*[^#\n]*registry\.gitlab\.com/progressiverobot/hmailserver[^\n]*\)[ \t]*;;", build, re.MULTILINE):
        problems.append("build/ci/linux-image.sh does not push to registry.gitlab.com/hmailserver/hmailserver alone: "
                        "it would push from the project's first path, whose image changes its address with the move")
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
                        ('https://gitlab.com/hmailserver/hmailserver//*) fail', "the id required at the group's path"),
                        ('for name in "$deb" "$deb.cdx.json"; do', "the .deb and its SBOM"),
                        ('"$api/packages/generic/hmailserver/$version/$name"', "from the release's own files")):
        if needle not in fetch:
            problems.append("build/ci/container-image-deb.sh no longer has %s (%s)" % (needle, why))
    # busybox's wget checks no server certificate: nothing it sends may be a credential.
    for number, line in enumerate(fetch.split("\n"), 1):
        if not line.lstrip().startswith("#") and re.search(r"TOKEN|PASSWORD|--header|Authorization", line, re.IGNORECASE):
            problems.append("build/ci/container-image-deb.sh line %d sends or names a credential, over a TLS its wget does not "
                            "check: %s" % (number, line.strip()))
    push = image_jobs.get("container-image", "")
    if push and not (re.search(r'^    HM_IMAGE_PUSH: "1"\s*$', push, re.MULTILINE) and "build/ci/linux-image.sh" in push):
        problems.append("container-image does not run build/ci/linux-image.sh with HM_IMAGE_PUSH=1")
    for name in ("container-image", "container-image-sbom"):
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
                        "the template's dependencies: [] leaves it no CS_IMAGE")
    image_script = texts["image-sign"]
    if TAG_SCRIPT_IDENTITY not in image_script:
        problems.append("build/ci/container-image-sign.sh expects another identity than %s" % TAG_SCRIPT_IDENTITY)
    image_flags = re.search(r'^sign_flags="([^"]*)"$', image_script, re.MULTILINE)
    if not image_flags or "--rekor-url https://rekor.sigstore.dev" not in image_flags.group(1):
        problems.append("build/ci/container-image-sign.sh signs without --rekor-url https://rekor.sigstore.dev (Rekor v1)")
    if "--use-signing-config=false" not in image_script:
        problems.append("build/ci/container-image-sign.sh no longer turns the signing configuration off")
    for needle, why in (('cosign attest $attest_flags --type cyclonedx --predicate "$sbom" "$image"', "the SBOM attested to the image"),
                        ('cosign verify-attestation $verify_flags --type cyclonedx --certificate-identity "$identity"', "the attestation verified as the release's identity"),
                        ("sbom=container-image-sbom/hmailserver-image.cdx.json", "the SBOM container-image-sbom made")):
        if needle not in image_script:
            problems.append("build/ci/container-image-sign.sh no longer has %s (%s): the image would go out without its SBOM" % (needle, why))
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
    # Every Linux file the release carries, the chart and the runtime's sources, with a
    # line in the release's sums: linux-build's own two, then sums_members.
    names_999 = feed.installer_names("9.9.9")
    unsummed = sorted(set(primaries) - set(chain.sums_members("9.9.9")) - {
        names_999["windows"], names_999["deb-amd64"], names_999["rpm-x86_64"], chain.sums_name("9.9.9"),
        "hmailserver.spdx.json", "hmailserver.cyclonedx.json"})
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

    # The feed pipeline runs pages and nothing else.
    update = jobs["update-feed"]
    project = feed.PROJECT_URL.split("/", 3)[3]
    if not re.search(r"^  trigger:\s*\n    project: %s\s*\n    branch: master\s*$" % re.escape(project), update, re.MULTILINE):
        problems.append("update-feed does not trigger a pipeline of %s on master" % project)
    if re.search(r"^  variables:", update, re.MULTILINE) or not re.search(r"^  inherit:\s*\n    variables: false\s*$", update, re.MULTILINE):
        problems.append("update-feed passes variables to the pipeline it starts (its own, or the file's without inherit: "
                        "variables: false): GitLab takes them as pipeline variables, which the project refuses by default, "
                        "and the trigger fails")
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
            problems.append("%s would run in the feed's pipeline (source pipeline, on master) or a schedule's: "
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
        if not re.search(r"^  tags: \[(linix|bench150)\]", inherited(name, jobs), re.MULTILINE):
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
    sast = blocks(texts["security"]).get("semgrep-sast", "")
    if "- if: " + TAG_ONLY not in sast:
        problems.append("semgrep-sast does not run for a protected v* tag (- if: %s): the release job needs its report, "
                        "and master, pushed with ci.skip, may never have scanned the tagged commit" % TAG_ONLY)
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
    if not os.path.isfile(os.path.join(ROOT, ".gitleaksignore")):
        problems.append(".gitleaksignore is missing: the findings read and found not to be secrets would fail secret_detection")
    return problems, notes


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


def load_texts():
    texts = dict((key, read(relative)) for key, relative in FILES.items())
    texts["tree"] = tree_files()
    return texts


def self_test(chain, feed):
    texts = load_texts()
    failures = []
    problems, notes = check(texts, chain, feed)
    if problems:
        failures.append("the tree passes: %s" % problems)

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
    broken("cosign from ghcr.io", "ci", 'HM_COSIGN_IMAGE: "gcr.io/projectsigstore/cosign:', 'HM_COSIGN_IMAGE: "ghcr.io/sigstore/cosign:', "never ghcr.io")
    broken("the old bundle format", "sign", 'sign_flags="--yes --new-bundle-format', 'sign_flags="--yes', "--new-bundle-format")
    broken("another log", "sign", "--rekor-url https://rekor.sigstore.dev --fulcio", "--rekor-url https://log2025-1.rekor.sigstore.dev --fulcio", "Rekor v1 log")
    broken("the signing configuration left on", "sign", "--use-signing-config=false", "--use-signing-config=true", "signing configuration off")
    broken("another count of files", "sign", "expected=57", "expected=13", "a release carries")
    broken("verify counting another number of files", "verify", '[ "$verified" -eq 57 ]', '[ "$verified" -eq 13 ]', "release-verify.sh counts 13")
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
    broken("a job that would run in the feed's pipeline", "ci", '.not-a-fuzz-pipeline:\n  rules:\n    - if: ' + REFUSAL,
           '.not-a-fuzz-pipeline:\n  rules:\n    - if: $CI_PIPELINE_SOURCE == "schedule"', "checkers would run in the feed's pipeline")
    broken("the feed's pipeline marked by a variable", "ci", "  inherit:\n    variables: false\n  trigger:",
           "  inherit:\n    variables: false\n  variables:\n    HM_PIPELINE: feed\n  trigger:", "update-feed passes variables")
    broken("the file's variables passed to the feed's pipeline", "ci", "  inherit:\n    variables: false\n  trigger:", "  trigger:",
           "update-feed passes variables")
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
    broken("a job on linix without the paths", "ci", "      changes:\n        paths: *not-docs-only\n      when: manual", "      when: manual",
           "db-matrix runs on the project's own runners for a push that changes only documentation")
    broken("a pattern GitLab would read otherwise", "ci", '          - "**/*[!d]"', '          - "**/*{[!d],x}"', "braces are not read here")
    # The gates.
    broken("the Windows suite cancelled by a later push", "ci", "  timeout: 4 hours\n  # A later push does not cut the suite off half-way, nor leave the bench mid-build.\n  interruptible: false",
           "  timeout: 4 hours", "windows-gate can be cancelled half-way")
    broken("a busy bench let past in a tag's pipeline", "ci", "if ($taken -eq 1 -and -not $env:CI_COMMIT_TAG) { \"the bench is busy - no gate",
           "if ($taken -eq 1) { \"the bench is busy - no gate", "windows-gate lets an exit code fail")
    broken("the cosign placeholder back", "ci", 'cosign:v2.6.5-dev@sha256:76029a0246bcfde96caa72469a62a1094f6e0978dc3f537f5cafd4c5f6c6a417"',
           'cosign:v2.6.5-dev@sha256:PIN-THE-DIGEST"', "is the placeholder")
    # The fuzz job's findings, and the secret scan's.
    broken("fuzz findings public", "ci", "    access: developer\n    paths:\n      - fuzz-results/", "    paths:\n      - fuzz-results/",
           "linux-fuzz does not keep fuzz-results/")
    broken("a finding's report printed in the public log", "fuzz", "2> /dev/null | head -n 1)\n",
           "2> /dev/null | head -n 1)\n    tail -n 60 \"$report\"\n", "prints a finding's report into the job log")
    broken("a finding's place in the public log", "fuzz", "| sed -E 's/^(SUMMARY:", "| cat | sed -E 's/^(SUMMARX:",
           "prints a sanitizer's SUMMARY whole")
    broken("fuzz findings claimed by another job", "ci", "    paths:\n      - security/repo-hygiene/", "    paths:\n      - fuzz-results/\n      - security/repo-hygiene/",
           "repo-hygiene claims fuzz-results/")
    broken("a secret found as a warning", "security", "  allow_failure: false\n", "", "secret_detection does not say allow_failure: false")
    broken("the analyzer's exit taken as the verdict", "security", "      if grep -Eq '\"vulnerabilities\" *: *\\[ *\\]' \"$report\"; then", "      if true; then",
           "secret_detection does not read its report")
    broken("the verifier trusting another file", "verifier", '//.gitlab-ci.yml@refs/tags/v"', '//.gitlab/ci.yml@refs/tags/v"', "GITLAB_IDENTITY_PREFIX")
    broken("the verifier trusting another group path", "verifier", 'GITLAB_GROUP_IDENTITY_PREFIX = "https://gitlab.com/hmailserver/hmailserver//',
           'GITLAB_GROUP_IDENTITY_PREFIX = "https://gitlab.com/hmailserver//', "GITLAB_GROUP_IDENTITY_PREFIX")
    broken("the group's signer with its id optional", "verifier", 'GITLAB_GROUP_REPOSITORY, GITLAB_REPOSITORY_ID, false, 0, ""',
           'GITLAB_GROUP_REPOSITORY, GITLAB_REPOSITORY_ID, true, 0, ""', "the project's id required")
    broken("the group's signer on another id", "verifier", 'GITLAB_GROUP_REPOSITORY, GITLAB_REPOSITORY_ID, false',
           'GITLAB_GROUP_REPOSITORY, GITHUB_REPOSITORY_ID, false', "the project's id required")
    broken("a script signing from a third path", "verify", "  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;\n",
           "  hmailserver/hmailserver) repository=https://gitlab.com/hmailserver/hmailserver ;;\n  someone/hmailserver) repository=https://gitlab.com/someone/hmailserver ;;\n",
           "release-verify.sh accepts the paths")
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
        chain.SIGNING_REPOSITORIES = dict(held_paths, **{"someone/hmailserver": "https://gitlab.com/someone/hmailserver"})
        found, _ = check(texts, chain, feed)
        if not any("signs its provenance for" in p for p in found):
            failures.append("a provenance identity for a third path is caught: %s" % found)
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
    broken("the group's id left to the settings", "verifier", "         else if (!groupRefusal.IsEmpty())\n            why = groupRefusal;\n", "",
           "whatever the settings")
    broken("a signature without a timestamp", "authenticode", "/tr $timestampUrl /td SHA256", "/td SHA256", "RFC 3161 timestamp")
    broken("the .deb taken before the release is published", "image", "    - job: release-publish\n      artifacts: false\n", "",
           "container-image-deb does not need release-publish")
    broken("the image pushed for a branch", "image", "    - !reference [.release-rules, rules]\n  variables:\n    HM_IMAGE_PUSH",
           "    - if: $CI_COMMIT_BRANCH\n  variables:\n    HM_IMAGE_PUSH", "container-image can run for something that is not a protected v* tag")
    broken("the image built before the .deb is taken", "image", "  needs:\n    - job: container-image-deb\n      artifacts: true\n  timeout: 30 minutes",
           "  needs: []\n  timeout: 30 minutes", "container-image does not need container-image-deb")
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
    broken("the image built from release-sign's artifact", "image", "    HM_IMAGE_PUSH: \"1\"\n    HM_IMAGE_DEB_DIR: container-image-deb\n",
           "    HM_IMAGE_PUSH: \"1\"\n    HM_IMAGE_DEB_DIR: release-signed\n", "container-image does not take the .deb from container-image-deb")
    broken("the .deb taken unverified", "image-deb", '--certificate-identity "$candidate" --certificate-oidc-issuer "$issuer"',
           '--certificate-identity-regexp ".*" --certificate-oidc-issuer-regexp ".*"', "as this project's pipeline")
    broken("the group's path without the project's id", "image-deb", "https://gitlab.com/hmailserver/hmailserver//*) fail", "nothing) fail",
           "the id required at the group's path")
    broken("a token sent where TLS is not checked", "image-deb", 'wget -q -U hmailserver-container-image',
           'wget -q --header "JOB-TOKEN: $CI_JOB_TOKEN" -U hmailserver-container-image', "sends or names a credential")
    broken("the image started otherwise in the job", "image", "--apply-new-install-defaults && exec", "--foreground || exec",
           "does not run build/ci/container-image-start-test.sh's START_COMMAND")
    broken("the start test without a network of its own", "image", '    FF_NETWORK_PER_BUILD: "true"\n', "",
           "container-image-start has no network of its own")
    broken("the start test on another image", "image", "    - name: $HM_IMAGE\n", "    - name: registry.gitlab.com/hmailserver/hmailserver:latest\n",
           "container-image-start does not start the pushed image by its digest")
    broken("the switch turned on in the job", "image", "  variables:\n    HM_IMAGE_PUSH: \"1\"\n",
           "  variables:\n    HM_PUBLISH_IMAGE: \"1\"\n    HM_IMAGE_PUSH: \"1\"\n", "sets HM_PUBLISH_IMAGE")
    broken("the switch turned on for every job", "ci", "\nvariables:\n", "\nvariables:\n  HM_PUBLISH_IMAGE: \"1\"\n", "sets HM_PUBLISH_IMAGE")
    broken("the image signed for another audience", "image", "      aud: sigstore", "      aud: fulcio", "container-image-sign has no id_tokens")
    broken("the image signed before it is pushed", "image", "  needs:\n    - job: container-image\n      artifacts: true\n    - job: container-image-sbom\n      artifacts: true\n  timeout: 15 minutes",
           "  needs: []\n  timeout: 15 minutes", "container-image-sign does not need container-image")
    broken("the scan without the image's name", "image", "  dependencies: [container-image]\n", "", "container_scanning does not take")
    broken("the image signed before its SBOM is made", "image", "    - job: container-image-sbom\n      artifacts: true\n", "",
           "container-image-sign does not need container-image-sbom")
    broken("the image's SBOM made from release-sign's artifact", "image", "  variables:\n    HM_IMAGE_DEB_DIR: container-image-deb\n  script:\n    - bash",
           "  variables:\n    HM_IMAGE_DEB_DIR: release-signed\n  script:\n    - bash", "container-image-sbom does not take the .deb from container-image-deb")
    broken("the image signed without its SBOM attested", "image-sign", 'cosign attest $attest_flags --type cyclonedx --predicate "$sbom" "$image"',
           'true', "the image would go out without its SBOM")
    broken("the image file not included", "ci", "  - local: .gitlab/ci/container-image.yml\n", "", "does not include .gitlab/ci/container-image.yml")
    broken("the image signed as a branch", "image-sign", TAG_SCRIPT_IDENTITY,
           'identity="${repository}//.gitlab-ci.yml@refs/heads/${tag}"', "container-image-sign.sh expects another identity")
    # The security scans, before the release.
    broken("a release that does not wait for GitLab's SAST", "ci", "    - job: semgrep-sast\n      artifacts: true\n", "",
           "release does not need semgrep-sast")
    broken("a release that does not bring the dependency scan's report", "ci", "    - job: dependency-scan\n      artifacts: true\n",
           "    - job: dependency-scan\n      artifacts: false\n", "release does not bring dependency-scan's artifacts")
    broken("a release that needs a scan optionally", "ci", "    - job: static-analysis\n      artifacts: true\n",
           "    - job: static-analysis\n      artifacts: true\n      optional: true\n", "release needs static-analysis optionally")
    broken("GitLab's SAST not run for a release tag", "security",
           '    - if: $CI_COMMIT_TAG =~ /^v/ && $CI_COMMIT_REF_PROTECTED == "true"\n  # The report as a plain file',
           "  # The report as a plain file", "semgrep-sast does not run for a protected v* tag")
    broken("GitLab's SAST report kept as a report only", "security", "    paths:\n      - gl-sast-report.json\n", "",
           "semgrep-sast does not keep gl-sast-report.json")
    broken("collect not asking the gate", "chain",
           '    problems.extend(security_gate.evaluate(root, root, exceptions_text=exceptions or "exceptions: []\\n", say=say))\n', "",
           "no longer asks build/ci/security-gate.py")
    broken("the gate given the tagged tree's exceptions", "chain", 'exceptions_text=exceptions or "exceptions: []\\n", say=say)',
           "say=say)", "does not give the gate master's")
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
    broken("the gathering job on the shared runners", "release-packages", "  extends: .linix\n", "  image: python:3\n",
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
    broken("the SBOM count left at 5", "verify", '[ "$described" -eq 26 ]', '[ "$described" -eq 5 ]', "holds 5 file(s) to their SBOM")
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
            lambda version: held_expected(version) + ["hMailServer-%s-arm64.exe" % version], "each of them needs its SBOM")
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


def main():
    feed = _load(os.path.join("build", "generate-update-feed.py"), "generate_update_feed")
    chain = _load(os.path.join("build", "ci", "release-chain.py"), "release_chain")
    if "--self-test" in sys.argv[1:]:
        return self_test(chain, feed)
    problems, notes = check(load_texts(), chain, feed)
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
