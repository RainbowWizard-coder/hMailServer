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
  * the release job's tag check (build/ci/release-chain.py collect), given a git
    that answers as the job's checkout would, accepts a tag signed by a key the
    tagged tree's own .gitlab/allowed_signers lists and master's does not, a tag
    on a commit that is not on master, or a tag while master's list holds a
    second key, or refuses a tag master's one key signed on master's history; or
    the tree's .gitlab/allowed_signers lists more than one key, which the release
    job refuses as master's (release tags are the owner's alone, GOVERNANCE.md);
  * collect, given a registry that holds files of the release's version, takes
    one with other bytes, or the installer or a bundle, which only a signing
    after it makes, or refuses one with the same bytes; or publish uploads before
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
    renovate and secret_detection (.gitlab/ci/gitlab-security.yml, the whole
    history) each run for the schedule described "fuzz", "renovate" or "security"
    and pages refuses those three; or a rule of either file still names
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
    its report after the analyzer, which exits 0 whatever it finds.
  * the container image's jobs (.gitlab/ci/container-image.yml) can run for
    anything but a protected v* tag, push before release-publish, sign before the
    push, sign without an ID token for Sigstore or in another image than
    HM_COSIGN_IMAGE, or build/ci/container-image-sign.sh expects another identity
    or log than the release's files are signed with.

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
}

RELEASE_JOBS = ["windows-installer", "release", "release-authenticode", "release-sign", "release-verify",
                "release-publish", "update-feed"]
# The container image's jobs, in .gitlab/ci/container-image.yml, and what each must
# need: the image is pushed once the release is published, from the .deb release-sign
# signed, and signed and scanned once it is pushed.
IMAGE_JOBS = {
    "container-image": ["release-sign", "release-verify", "release-publish"],
    "container-image-sign": ["container-image"],
    "container_scanning": ["container-image"],
}
# Jobs that need not begin with REFUSAL, because their own rules match nothing the
# feed's pipeline (source "pipeline", on master) is - each with the text that makes
# it so. None of them is a gate of a tag's pipeline, so release need not wait for
# them either.
EXEMPT = {
    "pages": ("$CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH", "the feed pipeline's one job"),
    "linux-fuzz": ('$CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "fuzz" &&', "only the fuzz schedule"),
    "renovate": ('$CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "renovate" &&', "only Renovate's schedule"),
    "sign-off": ('$CI_PIPELINE_SOURCE == "merge_request_event" &&', "only a merge request"),
    "installer-smoke": ("($CI_COMMIT_TAG =~ /^v/ || $CI_COMMIT_BRANCH =~ /^batch/)", "only a v* tag or a batch* branch"),
}
TAG_ONLY = '$CI_COMMIT_TAG =~ /^v/ && $CI_COMMIT_REF_PROTECTED == "true"'
# What every job but EXEMPT's begins its rules with, when: never: every scheduled
# pipeline, and the feed's downstream pipeline, whose CI_PIPELINE_SOURCE is "pipeline".
REFUSAL = '$CI_PIPELINE_SOURCE == "schedule" || $CI_PIPELINE_SOURCE == "pipeline"'
# The schedules that each run one job of their own, by their description, and the
# file the job is in. pages, which runs for every other schedule on master, refuses these.
ONE_JOB_SCHEDULES = {"fuzz": ("linux-fuzz", "ci"), "renovate": ("renovate", "ci"), "security": ("secret_detection", "security")}
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
    digests = dict((name, "a" * 64) for name in chain.expected_assets(version))

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
        ("a bundle, which Sigstore makes after collect", Registry(("hmailserver.spdx.json.cosign.bundle", "c" * 64)), True),
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
        if re.search(r"^  trigger:", image_jobs[name], re.MULTILINE):
            problems.append("%s is a trigger: a child or downstream pipeline's ID token has no ci_config_ref_uri" % name)
        have = needs(image_jobs[name])
        for need in wanted:
            if need not in have:
                problems.append("%s does not need %s (it needs %s)" % (name, need, ", ".join(have) or "nothing"))
    push = image_jobs.get("container-image", "")
    if push and not (re.search(r'^    HM_IMAGE_PUSH: "1"\s*$', push, re.MULTILINE) and "build/ci/linux-image.sh" in push):
        problems.append("container-image does not run build/ci/linux-image.sh with HM_IMAGE_PUSH=1")
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
    if SCRIPT_IDENTITY not in image_script:
        problems.append("build/ci/container-image-sign.sh expects another identity than %s" % SCRIPT_IDENTITY)
    image_flags = re.search(r'^sign_flags="([^"]*)"$', image_script, re.MULTILINE)
    if not image_flags or "--rekor-url https://rekor.sigstore.dev" not in image_flags.group(1):
        problems.append("build/ci/container-image-sign.sh signs without --rekor-url https://rekor.sigstore.dev (Rekor v1)")
    if "--use-signing-config=false" not in image_script:
        problems.append("build/ci/container-image-sign.sh no longer turns the signing configuration off")

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
        owner = jobs if where == "ci" else security_jobs
        if chosen not in owner.get(job, ""):
            problems.append("%s does not run for the schedule described \"%s\" (%s): a schedule carries no variable on this "
                            "project, so its description is what chooses the job" % (job, description, chosen))
        refused = re.search(r'- if: \$CI_PIPELINE_SOURCE == "schedule" && \(([^\n]*)\)\n\s+when: never', jobs["pages"])
        if not refused or '$CI_PIPELINE_SCHEDULE_DESCRIPTION == "%s"' % description not in refused.group(1):
            problems.append("pages would run in the pipeline of the schedule described \"%s\" too, which is %s's alone: "
                            "it refuses a schedule of that description, when: never" % (description, job))
    for key, found in (("ci", jobs), ("security", security_jobs)):
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
        if name.startswith(".") or name in RESERVED or name in RELEASE_JOBS or name in ("linux-fuzz", "renovate"):
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
    broken("another count of files", "sign", "expected=6", "expected=5", "a release carries")
    broken("a release started without a person", "ci", "      when: manual\n      allow_failure: false\n  before_script:",
           "      allow_failure: false\n  before_script:", "must wait for a person")
    broken("Sigstore before Authenticode", "ci", "    - job: release-authenticode\n      artifacts: true\n  timeout: 30 minutes\n  interruptible: false\n  rules:\n    - !reference [.release-rules, rules]\n  id_tokens:\n    SIGSTORE_ID_TOKEN",
           "  timeout: 30 minutes\n  interruptible: false\n  rules:\n    - !reference [.release-rules, rules]\n  id_tokens:\n    SIGSTORE_ID_TOKEN", "release-sign does not need release-authenticode")
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
    broken("pages in the fuzz schedule's pipeline", "ci", '($CI_PIPELINE_SCHEDULE_DESCRIPTION == "fuzz" || ', "(",
           "pages would run in the pipeline of the schedule described \"fuzz\"")
    broken("pages in the history scan's pipeline", "ci", ' || $CI_PIPELINE_SCHEDULE_DESCRIPTION == "security")', ")",
           "pages would run in the pipeline of the schedule described \"security\"")
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
    broken("a script expecting a branch's identity", "sign", 'identity="${repository}//.gitlab-ci.yml@refs/tags/${tag}"',
           'identity="${repository}//.gitlab-ci.yml@refs/heads/${tag}"', "release-sign.sh expects another identity")
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
    broken("the image pushed before the release is published", "image", "    - job: release-publish\n      artifacts: false\n", "",
           "container-image does not need release-publish")
    broken("the image pushed for a branch", "image", "  rules:\n    - !reference [.release-rules, rules]\n  variables:\n    HM_IMAGE_PUSH",
           "  rules:\n    - if: $CI_COMMIT_BRANCH\n  variables:\n    HM_IMAGE_PUSH", "container-image can run for something that is not a protected v* tag")
    broken("the image signed for another audience", "image", "      aud: sigstore", "      aud: fulcio", "container-image-sign has no id_tokens")
    broken("the image signed before it is pushed", "image", "  needs:\n    - job: container-image\n      artifacts: true\n  timeout: 15 minutes",
           "  needs: []\n  timeout: 15 minutes", "container-image-sign does not need container-image")
    broken("the scan without the image's name", "image", "  dependencies: [container-image]\n", "", "container_scanning does not take")
    broken("the image file not included", "ci", "  - local: .gitlab/ci/container-image.yml\n", "", "does not include .gitlab/ci/container-image.yml")
    broken("the image signed as a branch", "image-sign", 'identity="${repository}//.gitlab-ci.yml@refs/tags/${tag}"',
           'identity="${repository}//.gitlab-ci.yml@refs/heads/${tag}"', "container-image-sign.sh expects another identity")
    broken("signing in a child pipeline", "ci", "  script:\n    - sh build/ci/release-sign.sh", "  trigger:\n    include: child.yml\n  script:\n    - sh build/ci/release-sign.sh", "release-sign is a trigger")

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
    patched("collect taking whatever the registry holds", "check_registry_before_signing", lambda api, version, digests: [], "collect takes a registry holding")
    held_registry = chain.check_registry_before_signing
    patched("collect taking an installer already signed", "check_registry_before_signing",
            lambda api, version, digests: [p for p in held_registry(api, version, digests) if "earlier run's signing" not in p],
            "holding the installer")
    patched("collect refusing the same bytes", "check_registry_before_signing",
            lambda api, version, digests: held_registry(api, version, dict((n, "f" * 64) for n in digests)), "collect refuses a registry holding the Linux files")

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
