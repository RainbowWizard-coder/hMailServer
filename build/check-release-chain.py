#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The release jobs of .gitlab-ci.yml are what a server built after 6.3.3 needs them to be.

A v* tag's pipeline ends in the release chain (docs/ContinuousIntegration.md, "The
release"): windows-installer, release, release-authenticode, release-sign,
release-verify, release-publish and update-feed. Most of what makes a release
installable is fixed in places that do not look like each other - the identity
SigstoreVerifier.cpp compiles in, the file the pipeline is defined in, the ID
token's audience, cosign's flags, the order of the jobs - and a change to any one
of them is a release no server installs, found only when the first server
refuses it. This checker holds them to each other. It fails when:

  * release-sign is not a job of this file with an ID token for Sigstore (aud
    sigstore), is a trigger (a child or downstream pipeline's token carries no
    ci_config_ref_uri, and Fulcio issues it no certificate), or runs in an image
    that is not cosign's own from gcr.io pinned by digest - the one placeholder
    the file ships with until the digest is established is reported as a NOTE;
  * build/ci/release-sign.sh stops naming the bundle format and the log a server
    reads (--new-bundle-format, Rekor v1 at rekor.sigstore.dev, the signing
    configuration off, hashedrekord 0.0.1), or counts another number of files than
    a release carries (build/ci/release-chain.py);
  * the identity the scripts expect is not the one SigstoreVerifier.cpp trusts,
    or that one is not this project's path, file and repository id; or the
    verifier stops trusting the group the project is to move to (MOVE_TO) with
    the project's id required, or the scripts sign from a path it does not trust;
  * release-authenticode has no ID token for Entra ID (aud
    api://AzureADTokenExchange), or its script signs without the service's RFC
    3161 timestamp, or reads other settings than the release job checks;
  * the release or release-publish job can start without a person (when: manual,
    allow_failure: false), a release job can run for anything but a protected v*
    tag, or the jobs' needs no longer put Authenticode before Sigstore, Sigstore
    before the verifier, and the verifier before the upload;
  * release does not wait for every job that can fail a tag's pipeline (every job
    that is neither allow_failure nor manual, nor one of the few that never runs in
    one), or brings the artifacts of any but the three whose files it collects;
  * the pinned-image pattern the release job holds HM_COSIGN_IMAGE to at run time
    (build/ci/release-chain.py) is not this checker's, which a CI/CD variable of
    the same name could otherwise take past;
  * update-feed is not a trigger of this project's master that passes no
    variables, or a job other than pages would run in the pipeline it starts: that
    pipeline's source is "pipeline", and every job refuses it (and any HM_PIPELINE,
    and a schedule), except the few whose own rules can only match something else,
    each named here with why. Its pipeline is told apart by its source because a
    trigger job's variables reach the downstream pipeline as pipeline variables,
    which GitLab refuses by default in a project made since 17.7.

Usage:
  python build/check-release-chain.py              the tree as it is
  python build/check-release-chain.py --self-test  each rule broken in a copy, and caught
"""

import importlib.util
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = {
    "ci": ".gitlab-ci.yml",
    "sign": os.path.join("build", "ci", "release-sign.sh"),
    "verify": os.path.join("build", "ci", "release-verify.sh"),
    "authenticode": os.path.join("build", "ci", "windows-authenticode.ps1"),
    "installer": os.path.join("build", "ci", "windows-installer.ps1"),
    "verifier": os.path.join("hmailserver", "source", "Server", "Common", "Util", "SigstoreVerifier.cpp"),
}

RELEASE_JOBS = ["windows-installer", "release", "release-authenticode", "release-sign", "release-verify",
                "release-publish", "update-feed"]
# Jobs that need not begin with REFUSAL, because their own rules match nothing the
# feed's pipeline (source "pipeline", on master) is - each with the text that makes
# it so. None of them is a gate of a tag's pipeline, so release need not wait for
# them either.
EXEMPT = {
    "pages": ("$CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH", "the feed pipeline's one job"),
    "linux-fuzz": ('$HM_PIPELINE == "fuzz" &&', "only the fuzz pipeline"),
    "renovate": ('$CI_PIPELINE_SOURCE == "schedule" &&', "only Renovate's schedule"),
    "sign-off": ('$CI_PIPELINE_SOURCE == "merge_request_event" &&', "only a merge request"),
    "installer-smoke": ("($CI_COMMIT_TAG =~ /^v/ || $CI_COMMIT_BRANCH =~ /^batch/)", "only a v* tag or a batch* branch"),
}
TAG_ONLY = '$CI_COMMIT_TAG =~ /^v/ && $CI_COMMIT_REF_PROTECTED == "true"'
# What every job but EXEMPT's begins its rules with, when: never: the fuzz schedule's
# pipeline and any other HM_PIPELINE, every scheduled pipeline, and the feed's
# downstream pipeline, whose CI_PIPELINE_SOURCE is "pipeline".
REFUSAL = '$HM_PIPELINE || $CI_PIPELINE_SOURCE == "schedule" || $CI_PIPELINE_SOURCE == "pipeline"'
# The jobs whose artifacts release collects; it waits for every other gate without them.
COLLECTED = ["linux-build", "linux-sbom", "windows-installer"]
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
SCRIPT_IDENTITY = 'identity="${repository}%s${tag}"' % IDENTITY_TAIL


def _load(relative, name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, relative))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read(relative):
    with io.open(os.path.join(ROOT, relative), encoding="utf-8") as f:
        return f.read().replace("\r\n", "\n")


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
    """Every job that can fail a v* tag's pipeline before release: not a release job,
    not EXEMPT (none of which is a gate of a tag), not allow_failure, not manual."""
    found = []
    for name in sorted(jobs):
        if name.startswith(".") or name in RESERVED or name in RELEASE_JOBS or name in EXEMPT:
            continue
        text = inherited(name, jobs) + "\n" + rules_text(name, jobs)
        if "allow_failure: true" in text or "when: manual" in text:
            continue
        found.append(name)
    return found


def need_entries(block):
    """A job's needs, each name with the text of its entry."""
    match = re.search(r"^  needs:\s*\n(.*?)(?=^  [a-z_]+:|\Z)", block, re.MULTILINE | re.DOTALL)
    entries = {}
    if not match:
        return entries
    for entry in re.split(r"^    - ", match.group(1), flags=re.MULTILINE):
        head = re.match(r"(?:job:\s*)?([a-z][a-z0-9\-]*)", entry)
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
        item = re.match(r"^\s*-\s*(?:job:\s*)?([a-z][a-z0-9\-]*)\s*$", line)
        if item:
            found.append(item.group(1))
    return found


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
        problems.append(".release-rules no longer holds the release jobs to a protected v* tag, refusing merge requests, any "
                        "HM_PIPELINE, a schedule and the feed's pipeline (%s)" % REFUSAL)
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
    order = {
        "release": gates(jobs) + ["windows-installer"],
        "release-authenticode": ["release"],
        "release-sign": ["release", "release-authenticode"],
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
        if name not in COLLECTED and not re.search(r"artifacts: false", entry):
            problems.append("release brings %s's artifacts, which it does not collect: say artifacts: false" % name)

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
        notes.append("HM_COSIGN_IMAGE is still the placeholder %s: the release job refuses to start the chain until the digest "
                     "is established (docs/ContinuousIntegration.md, 'The release')" % image.group(1))
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
    for needle, why in (("--use-signing-config=false", "the signing configuration off, which could choose Rekor v2"),
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
    if group_signer not in texts["verifier"]:
        problems.append("SigstoreVerifier.cpp's DefaultTrust no longer trusts the group's signer with the project's id required: %s" % group_signer)
    # The scripts name the identity after the project's path, which must be one of the
    # two the verifier trusts, each with its own repository, and nothing else.
    wanted = sorted([(feed.PROJECT_URL.split("/", 3)[3], feed.PROJECT_URL), (MOVE_TO.split("/", 3)[3], MOVE_TO)])
    for name in ("sign", "verify"):
        paths = sorted(re.findall(r"^  ([^\s)]+)\) repository=(\S+) ;;$", texts[name], re.MULTILINE))
        if paths != wanted:
            problems.append("%s accepts the paths %s; a server trusts a signature from %s alone" % (FILES[name], paths, wanted))
        if SCRIPT_IDENTITY not in texts[name] or '"${CI_PROJECT_PATH:-}"' not in texts[name]:
            problems.append("%s expects another identity than <the project's repository>%s<tag>" % (FILES[name], IDENTITY_TAIL))
    if not repository or repository.group(1) != feed.PROJECT_URL:
        problems.append("SigstoreVerifier.cpp's GITLAB_REPOSITORY is not %s" % feed.PROJECT_URL)
    if not repository_id or repository_id.group(1) != feed.PROJECT_ID or chain.PROJECT_ID != feed.PROJECT_ID:
        problems.append("SigstoreVerifier.cpp's GITLAB_REPOSITORY_ID is not the project id %s" % feed.PROJECT_ID)
    if not os.path.isfile(os.path.join(ROOT, ".gitlab-ci.yml")):
        problems.append("the CI file is not .gitlab-ci.yml at the root, the file every GitLab certificate names")

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
            problems.append("%s would run in the feed's pipeline (source pipeline, on master), a schedule's or the fuzz pipeline: "
                            "its rules must begin with - if: %s, when: never" % (name, REFUSAL))
    return problems, notes


def load_texts():
    return dict((key, read(relative)) for key, relative in FILES.items())


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
    broken("another count of files", "sign", "expected=6", "expected=5", "a release carries")
    broken("a release started without a person", "ci", "      when: manual\n      allow_failure: false\n  before_script:",
           "      allow_failure: false\n  before_script:", "must wait for a person")
    broken("Sigstore before Authenticode", "ci", "    - job: release-authenticode\n      artifacts: true\n  timeout: 30 minutes\n  interruptible: false\n  rules:\n    - !reference [.release-rules, rules]\n  id_tokens:\n    SIGSTORE_ID_TOKEN",
           "  timeout: 30 minutes\n  interruptible: false\n  rules:\n    - !reference [.release-rules, rules]\n  id_tokens:\n    SIGSTORE_ID_TOKEN", "release-sign does not need release-authenticode")
    broken("a job that would run in the feed's pipeline", "ci", '.not-a-fuzz-pipeline:\n  rules:\n    - if: ' + REFUSAL,
           '.not-a-fuzz-pipeline:\n  rules:\n    - if: $HM_PIPELINE || $CI_PIPELINE_SOURCE == "schedule"', "checkers would run in the feed's pipeline")
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
    broken("a release job for a branch", "ci", ".release-rules:\n  rules:\n    - if: $HM_PIPELINE", ".release-rules:\n  rules:\n    - if: $HM_PIPELINE_X", ".release-rules no longer holds")
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
    broken("a script expecting a branch's identity", "sign", 'identity="${repository}//.gitlab-ci.yml@refs/tags/${tag}"',
           'identity="${repository}//.gitlab-ci.yml@refs/heads/${tag}"', "release-sign.sh expects another identity")
    broken("a signature without a timestamp", "authenticode", "/tr $timestampUrl /td SHA256", "/td SHA256", "RFC 3161 timestamp")
    broken("signing in a child pipeline", "ci", "  script:\n    - sh build/ci/release-sign.sh", "  trigger:\n    include: child.yml\n  script:\n    - sh build/ci/release-sign.sh", "release-sign is a trigger")

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
