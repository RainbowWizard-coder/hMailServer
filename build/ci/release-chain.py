#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The two ends of a release's pipeline: what goes into it, and publishing it.

A v* tag's pipeline (.gitlab-ci.yml, the release stage; docs/ContinuousIntegration.md,
"The release") builds and tests, then waits for a maintainer to start the `release`
job. That job runs `collect`; Authenticode signs the installer, `release-sbom` runs
`sbom`, Sigstore signs every file, and `release-publish`, also started by hand, runs
`publish`:

  collect   the tag is an annotated tag, it still points at the commit this
            pipeline built, and that commit is on master; it is signed by the one
            key .gitlab/allowed_signers lists as master has it - never as the tag's
            own tree has it, which a tag on a commit of its own could write - and
            the job refuses while that list holds more than one key, since release
            tags are the owner's alone (GOVERNANCE.md, 26 September 2026);
            Version.h says the version the tag names, so every file name made from
            it is the name a server looks for; the cosign image is gcr.io's pinned
            by digest, whatever a CI/CD variable made it, and the Azure settings the Authenticode job reads are all there; the
            security scans of the commit finished, and every finding they raised that
            SECURITY.md's policy counts is in a scan's baseline or accepted in
            .gitlab/security-exceptions.yml (build/ci/security-gate.py); and the
            artefacts of the jobs it needs - the Windows installer, the .deb and the
            .rpm with their sums and each package's own SBOM, the two SBOMs of the
            tree - are gathered into one directory under the names a release
            carries, each package's SBOM held to the package it describes. The
            package hmailserver/<version> must not hold any of them with other bytes
            already, nor the installer, its SBOM or a bundle at all: those are made
            by the jobs after this one, and a second signing never makes the same
            bytes. Nothing leaves the pipeline.

  sbom      the installer's own SBOM (build/asset-sbom.py), made from the bytes
            Authenticode signed - every compiled file a release carries goes out
            with an SBOM of its own, <file>.cdx.json (OpenSSF Baseline
            OSPS-QA-02.02; SECURITY.md, "Software bills of materials").

  publish   every signed file and its <file>.cosign.bundle uploaded to the generic
            package hmailserver/<version> of this project, read back from the
            registry and held to the SHA-256 it was signed with - nothing is
            uploaded while the package holds a file of that name with other bytes,
            and a file it holds with the same bytes, from an earlier run of the job,
            is not uploaded again - once every compiled file's SBOM has been held
            to that file - then the GitLab
            release created with one link per file, named after the file, which is
            what build/generate-update-feed.py and every server require. The notes
            are the version's section of CHANGELOG.md and the verification the
            release-verify job wrote. Last, the feed is rendered from the API as the
            pages job will render it, and the new release must be in it with its
            installer and bundle for Windows, Debian and RPM servers.

Neither asks for a secret: the pipeline's own CI_JOB_TOKEN reads the registry, uploads
and creates the release, and it is sent as a header and never printed. A release that exists already
is refused - a tag is spent once - and so is any project but this one.

Usage, from the repository root:
  python3 build/ci/release-chain.py collect [--out release-assets]
  python3 build/ci/release-chain.py sbom [--out release-sbom]
  python3 build/ci/release-chain.py publish [--assets release-signed] [--verification release-verified/verification.md]
  python3 build/ci/release-chain.py --self-test      no network, no git, no token
"""

import argparse
import base64
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _load_feed():
    path = os.path.join(ROOT, "build", "generate-update-feed.py")
    spec = importlib.util.spec_from_file_location("generate_update_feed", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


feed = _load_feed()


def _load_asset_sbom():
    path = os.path.join(ROOT, "build", "asset-sbom.py")
    spec = importlib.util.spec_from_file_location("asset_sbom", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


asset_sbom = _load_asset_sbom()


def _load_gate():
    path = os.path.join(ROOT, "build", "ci", "security-gate.py")
    spec = importlib.util.spec_from_file_location("security_gate", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The security scans' verdict (build/ci/security-gate.py), which collect asks for
# before anything is signed.
security_gate = _load_gate()

PROJECT_ID = feed.PROJECT_ID
PACKAGE = "hmailserver"
API_BASE = "https://gitlab.com/api/v4"
# The identity Fulcio writes for this project's pipeline of tag <tag>, which
# SigstoreVerifier.cpp's GITLAB_IDENTITY_PREFIX accepts. build/check-release-chain.py
# holds the two to each other.
SIGNER_IDENTITY_PREFIX = "https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/"
SIGNER_ISSUER = "https://gitlab.com"
VERSION_H = os.path.join("hmailserver", "source", "Server", "Common", "Application", "Version.h")
ALLOWED_SIGNERS = ".gitlab/allowed_signers"
# The tag check reads the allow-list from here, fetched by name, and requires the
# tagged commit to be on it: master is protected, and a tag is not. The tagged
# tree's own .gitlab/allowed_signers is never read, since a tag on a commit that
# is not master's could carry a key of its own in it.
TRUSTED_BRANCH = "master"
TRUSTED_REF = "refs/remotes/origin/" + TRUSTED_BRANCH
# What git verify-tag prints for a good SSH signature: the principal and the
# fingerprint of the key that made it.
GOOD_SIGNATURE = re.compile(r'Good "git" signature for (\S+) with (\S+) key (SHA256:[A-Za-z0-9+/]+)')
CHANGELOG = "CHANGELOG.md"
# The settings release-authenticode reads (build/ci/windows-authenticode.ps1). Names
# only are ever printed.
AZURE_SETTINGS = ["AZURE_TENANT_ID", "AZURE_CLIENT_ID", "ARTIFACT_SIGNING_ENDPOINT",
                  "ARTIFACT_SIGNING_ACCOUNT", "ARTIFACT_SIGNING_PROFILE"]
# cosign's own image from gcr.io, pinned by digest: the rule build/check-release-chain.py
# holds the CI file's HM_COSIGN_IMAGE to (its PINNED_COSIGN, which it holds this to). The
# value is checked again here because a project or pipeline CI/CD variable of the same
# name replaces the file's at run time, and release-sign runs in whatever it names.
PINNED_IMAGE = re.compile(r"^gcr\.io/projectsigstore/cosign:[A-Za-z0-9._\-]+@sha256:[0-9a-f]{64}$")


class ReleaseError(Exception):
    pass


QUIET = []


def say(line):
    if not QUIET:
        print(line)


# ---------------------------------------------------------------- what a release carries

def version_of(tag):
    """The version a tag names: v6.3.5 -> 6.3.5. Refuses what is not a server tag."""
    if not tag or not feed.SERVER_TAG.match(tag):
        raise ReleaseError("%r is not a release tag: a release's tag is v, a digit, then digits, dots and a suffix "
                           "if it is a pre-release (v6.3.5, v6.3.5-rc1)" % tag)
    return tag[1:]


def sums_name(version):
    return "hmailserver-linux-%s-SHA256SUMS.txt" % version


def sbom_name(name):
    """The SBOM of one compiled file: <file>.cdx.json, beside it (build/asset-sbom.py)."""
    return name + asset_sbom.SUFFIX


def compiled_assets(version):
    """The compiled files a release carries: the installer and the two packages. Each
    goes out with an SBOM of its own (OpenSSF Baseline OSPS-QA-02.02)."""
    names = feed.installer_names(version)
    return [names["windows"], names["deb-amd64"], names["rpm-x86_64"]]


def expected_assets(version):
    """Every file a release carries, each signed beside it. The Linux AArch64
    packages, the per-distribution packages, the AppImages and the ARM64 installer
    have jobs of their own (.gitlab/ci/packages-*.yml), and no release collects what
    they make yet (docs/PackagesDeb.md, "What is left")."""
    names = feed.installer_names(version)
    return [names["windows"], names["deb-amd64"], names["rpm-x86_64"], sums_name(version),
            "hmailserver.spdx.json", "hmailserver.cyclonedx.json"] + [sbom_name(n) for n in compiled_assets(version)]


def made_after_collect(version):
    """What the jobs after collect make: the installer's signed bytes (Authenticode) and
    the installer's SBOM, which describes those bytes (release-sbom)."""
    installer = feed.installer_names(version)["windows"]
    return [installer, sbom_name(installer)]


def collected_assets(version):
    """What collect gathers: every file but the installer's SBOM, which release-sbom makes."""
    later = made_after_collect(version)[1:]
    return [name for name in expected_assets(version) if name not in later]


def sources(version):
    """Where collect finds each file: the artifacts of the jobs `release` needs."""
    names = feed.installer_names(version)
    return {
        names["windows"]: os.path.join("release-windows", names["windows"]),
        names["deb-amd64"]: os.path.join("linux-build", "packages", names["deb-amd64"]),
        names["rpm-x86_64"]: os.path.join("linux-build", "packages", names["rpm-x86_64"]),
        sbom_name(names["deb-amd64"]): os.path.join("linux-build", "packages", sbom_name(names["deb-amd64"])),
        sbom_name(names["rpm-x86_64"]): os.path.join("linux-build", "packages", sbom_name(names["rpm-x86_64"])),
        sums_name(version): os.path.join("linux-build", "packages", "SHA256SUMS.txt"),
        "hmailserver.spdx.json": os.path.join("sbom", "hmailserver.spdx.json"),
        "hmailserver.cyclonedx.json": os.path.join("sbom", "hmailserver.cyclonedx.json"),
    }


# The component list release-sbom makes the installer's SBOM from, which linux-sbom
# writes and the release job keeps in release-tools/ (build/ci/linux-sbom.sh).
DEPENDENCY_LIST = "native-dependencies-x64.json"


def sbom_problems(directory, version, names=None):
    """Each compiled file's SBOM in directory describes that very file, by its SHA-256,
    and is well formed; problems as sentences."""
    problems = []
    for name in names or compiled_assets(version):
        document = os.path.join(directory, sbom_name(name))
        asset = os.path.join(directory, name)
        if not os.path.isfile(document):
            problems.append("%s goes out without its SBOM, %s: every compiled file a release carries has one "
                            "(SECURITY.md, 'Software bills of materials')" % (name, sbom_name(name)))
            continue
        lines = []
        try:
            answer = asset_sbom.verify(document, asset, lines.append)
        except (OSError, asset_sbom.SbomError) as e:
            answer, lines = 1, ["PROBLEM: %s" % e]
        if answer != 0:
            problems.append("%s does not describe %s: %s" % (sbom_name(name), name,
                                                          "; ".join(l for l in lines if l.startswith("PROBLEM")) or "unreadable"))
    return problems


def is_primary(name):
    return not name.endswith(".cosign.bundle")


def permalink_name(name, version):
    """The release's permanent download name, as every release since 6.2.19 has
    had it: the version taken out of a package's name (hMailServer-x64.exe,
    hmailserver_amd64.deb, hmailserver.x86_64.rpm); a bundle and an SBOM keep theirs."""
    if not is_primary(name) or name.startswith("hmailserver.") and name.endswith(".json"):
        return name
    for marker, replacement in (("-%s-1." % version, "."), ("-%s" % version, ""), ("_%s" % version, "")):
        if marker in name:
            return name.replace(marker, replacement, 1)
    return name


def link_type(name):
    return "package" if is_primary(name) and name.endswith((".exe", ".deb", ".rpm", ".AppImage")) else "other"


def registry_url(version, name):
    return "%s/packages/generic/%s/%s/%s" % (feed.API, PACKAGE, urllib.parse.quote(version, safe=""), urllib.parse.quote(name, safe=""))


def release_links(version, names):
    links = []
    for name in sorted(names):
        links.append({"name": name, "url": registry_url(version, name), "link_type": link_type(name),
                      "direct_asset_path": "/downloads/" + permalink_name(name, version)})
    return links


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_version_h(root):
    with open(os.path.join(root, VERSION_H), encoding="utf-8") as f:
        text = f.read()
    match = re.search(r'^#define HMAILSERVER_VERSION "([^"]+)"', text, re.MULTILINE)
    if not match:
        raise ReleaseError("%s defines no HMAILSERVER_VERSION" % VERSION_H)
    return match.group(1)


def changelog_section(text, version):
    """The body of CHANGELOG.md's section for version, without its heading."""
    lines = text.replace("\r\n", "\n").split("\n")
    heading = re.compile(r"^## \[" + re.escape(version) + r"\]\(")
    for index, line in enumerate(lines):
        if heading.match(line):
            body = []
            for following in lines[index + 1:]:
                if following.startswith("## ") or following.strip() == "---":
                    break
                body.append(following)
            text = "\n".join(body).strip()
            if text:
                return text
            break
    raise ReleaseError("CHANGELOG.md has no section '## [%s](...)' with anything in it: the release's notes are that "
                       "section (RELEASE.md, the release on GitLab), so write it and tag again" % version)


# ---------------------------------------------------------------- collect

def signer_keys(text):
    """The keys an allowed_signers file lists: (principals, key type, key), one per
    line that is not blank or a comment. Options such as namespaces="git" may stand
    between the principals and the key."""
    keys = []
    for line in text.replace("\r\n", "\n").split("\n"):
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        for index, field in enumerate(fields[1:], 1):
            if re.match(r"^(ssh-|ecdsa-|sk-)", field) and index + 1 < len(fields):
                keys.append((fields[0], field, fields[index + 1]))
                break
        else:
            keys.append((fields[0], "", ""))
    return keys


def fingerprint(key):
    """The SHA256 fingerprint ssh-keygen -l prints for a public key given as base64;
    nothing for a key that is missing or not base64."""
    if not key:
        return ""
    try:
        blob = base64.b64decode(key, validate=True)
    except (ValueError, TypeError):
        return ""
    return "SHA256:" + base64.b64encode(hashlib.sha256(blob).digest()).decode("ascii").rstrip("=")


def check_tag(root, tag, commit, run=subprocess.run):
    """The tag is an annotated tag object; it points at the commit this pipeline built,
    so a tag deleted and made again on another commit after the pipeline started is
    refused, not signed for; that commit is on master; and the tag is signed by the
    one key master's .gitlab/allowed_signers lists.

    The allow-list is master's, fetched by name, and never the tagged tree's: a tag
    on a commit of its own could carry a key of its own in that file. It must list
    one key, since release tags are the owner's alone (GOVERNANCE.md), and the
    signature must be that key's by its fingerprint as well as by git's word. This
    runs as the tagged commit has it, so what it adds is defence in depth - a
    commit that is not master's can carry another release-chain.py too. Against
    such a commit the protected tag rule holds the Sigstore signature, and the
    Azure credential issued for one tag holds the Authenticode signature alone
    (RELEASE.md)."""
    problems = []

    def git(*arguments):
        return run(["git", "-C", root] + list(arguments), capture_output=True, text=True)

    def said(answer):
        return (answer.stderr or answer.stdout or "exit %d" % answer.returncode).strip()[:400]

    git("fetch", "--force", "--no-tags", "origin", "+refs/tags/%s:refs/tags/%s" % (tag, tag))
    kind = git("cat-file", "-t", "refs/tags/" + tag)
    if kind.returncode != 0 or kind.stdout.strip() != "tag":
        problems.append("%s is not an annotated tag (git cat-file -t says %r): a release's tag is made with "
                        "git tag -s %s -m '...' (RELEASE.md)" % (tag, kind.stdout.strip() or kind.stderr.strip(), tag))
        return problems
    pointed = git("rev-parse", "--verify", "--quiet", "refs/tags/%s^{commit}" % tag)
    target = pointed.stdout.strip() if pointed.returncode == 0 else ""
    if not re.match(r"^[0-9a-f]{40}([0-9a-f]{24})?$", commit or ""):
        problems.append("CI_COMMIT_SHA is %r, not a commit: collect runs in the release job of a v* tag's pipeline and "
                        "nowhere else" % (commit or ""))
    elif target != commit:
        problems.append("%s points at %s now, and this pipeline built %s: the tag was moved after the pipeline started. "
                        "Nothing has been signed; a release is made by the pipeline of the tag as it stands"
                        % (tag, target or "nothing", commit))

    # master, fetched by name. In the job's shallow checkout git fetches master's
    # history down to the tagged commit when it is an ancestor. Another shallow
    # boundary in the checkout can still hide the way down, so a "no" from a shallow
    # repository is asked again with master's whole history.
    fetched = git("fetch", "--no-tags", "origin", "+refs/heads/%s:%s" % (TRUSTED_BRANCH, TRUSTED_REF))
    if fetched.returncode != 0:
        problems.append("%s could not be fetched, and the allow-list and the tag's ancestry are read from it: %s"
                        % (TRUSTED_BRANCH, said(fetched)))
        return problems
    if target:
        ancestry = git("merge-base", "--is-ancestor", target, TRUSTED_REF)
        if ancestry.returncode == 1 and git("rev-parse", "--is-shallow-repository").stdout.strip() == "true":
            deepened = git("fetch", "--unshallow", "--no-tags", "origin", "+refs/heads/%s:%s" % (TRUSTED_BRANCH, TRUSTED_REF))
            if deepened.returncode != 0:
                problems.append("%s's whole history could not be fetched to ask whether %s is on it: %s"
                                % (TRUSTED_BRANCH, target, said(deepened)))
                return problems
            ancestry = git("merge-base", "--is-ancestor", target, TRUSTED_REF)
        if ancestry.returncode == 1:
            problems.append("%s points at %s, which is not on %s: a release is tagged on a commit %s already has "
                            "(RELEASE.md, 'Tag the release on master')" % (tag, target, TRUSTED_BRANCH, TRUSTED_BRANCH))
        elif ancestry.returncode != 0:
            problems.append("git could not say whether %s is on %s: %s" % (target, TRUSTED_BRANCH, said(ancestry)))

    listed = git("show", "%s:%s" % (TRUSTED_REF, ALLOWED_SIGNERS))
    if listed.returncode != 0:
        problems.append("%s has no %s to verify the tag against: %s" % (TRUSTED_BRANCH, ALLOWED_SIGNERS, said(listed)))
        return problems
    keys = signer_keys(listed.stdout)
    if len(keys) != 1 or not fingerprint(keys[0][2]):
        problems.append("%s's %s lists %d key(s)%s: release tags are signed by the owner's key alone (GOVERNANCE.md), so "
                        "the job takes a list of exactly one" % (TRUSTED_BRANCH, ALLOWED_SIGNERS, len(keys),
                                                                 "" if len(keys) != 1 else ", and its key is not base64"))
        return problems
    principal, _, key = keys[0]
    wanted = fingerprint(key)

    work = tempfile.mkdtemp(prefix="hm-signers-")
    try:
        signers = os.path.join(work, "allowed_signers")
        with open(signers, "w", encoding="utf-8", newline="\n") as f:
            f.write(listed.stdout)
        verified = git("-c", "gpg.ssh.allowedSignersFile=" + signers.replace(os.sep, "/"), "verify-tag", tag)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if verified.returncode != 0:
        problems.append("%s is not signed by the key %s's %s lists (%s, %s): %s" % (
            tag, TRUSTED_BRANCH, ALLOWED_SIGNERS, principal, wanted, said(verified)))
        return problems
    good = GOOD_SIGNATURE.search((verified.stderr or "") + "\n" + (verified.stdout or ""))
    if not good or good.group(3) != wanted or good.group(1) != principal:
        problems.append("%s is signed by %s, not by the one key %s's %s lists (%s, %s)" % (
            tag, "%s with %s" % (good.group(1), good.group(3)) if good else "a key git does not name", TRUSTED_BRANCH,
            ALLOWED_SIGNERS, principal, wanted))
    return problems


def check_sums(sums_path, deb_path, rpm_path):
    """linux-build's SHA256SUMS.txt names exactly the two packages, with their digests."""
    wanted = {os.path.basename(deb_path): sha256_of(deb_path), os.path.basename(rpm_path): sha256_of(rpm_path)}
    listed = {}
    with open(sums_path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            match = re.match(r"^([0-9a-f]{64})\s+\*?(\S+)$", line)
            if not match:
                return ["%s has a line that is not '<sha256>  <file>': %r" % (sums_path, line)]
            listed[match.group(2)] = match.group(1)
    if listed != wanted:
        return ["%s is not the sums of the two packages the release carries: it lists %s" % (sums_path, sorted(listed))]
    return []


def master_exceptions(root, run=subprocess.run):
    """(master's .gitlab/security-exceptions.yml, None), or (None, the problem). check_tag
    fetched master by name; the tagged tree's copy is never read, as its allow-list is not."""
    shown = run(["git", "-C", root, "show", "%s:%s" % (TRUSTED_REF, security_gate.EXCEPTIONS)], capture_output=True, text=True)
    if shown.returncode != 0:
        return None, ("%s's %s could not be read (master not fetched, or without the file), and it says what findings "
                      "of the security scans a release may go out with: the gate reads master's (SECURITY.md, 'Before a "
                      "release'): %s" % (
                          TRUSTED_BRANCH, security_gate.EXCEPTIONS, (shown.stderr or shown.stdout or "").strip()[:300]))
    return shown.stdout, None


def collect(root, tag, out, env, run=subprocess.run, api=None):
    """Checks everything the rest of the chain relies on, then gathers the files.
    Returns the problems; the files are copied only when there are none."""
    problems = []
    try:
        version = version_of(tag)
    except ReleaseError as e:
        return [str(e)]

    stamped = read_version_h(root)
    if stamped != version:
        problems.append("Version.h says %s and the tag says %s: every file name is made from Version.h, so the release "
                        "would carry names no server looks for (RELEASE.md, the version stamp)" % (stamped, version))

    image = env.get("HM_COSIGN_IMAGE", "")
    if not PINNED_IMAGE.match(image):
        problems.append("HM_COSIGN_IMAGE is %r, not gcr.io/projectsigstore/cosign pinned by digest: set the digest of "
                        "its -dev tag in .gitlab-ci.yml (docs/ContinuousIntegration.md, \"The release\"), and remove any "
                        "CI/CD variable of that name, which replaces the file's value" % image)

    missing = [name for name in AZURE_SETTINGS if not (env.get(name) or "").strip()]
    if missing:
        problems.append("the Authenticode job needs the CI/CD variables %s, which are not set for this tag's pipeline "
                        "(RELEASE.md, \"Authenticode\"). Nothing has been signed." % ", ".join(missing))

    problems.extend(check_tag(root, tag, env.get("CI_COMMIT_SHA", ""), run))

    # The security scans of this commit, read before anything is signed: static-analysis,
    # semgrep, semgrep-sast and dependency-scan, whose artifacts the job needs. A scan that
    # did not finish, or a finding the policy counts that is in no baseline and no
    # exception in force, holds the release (SECURITY.md, "Before a release"). The
    # exceptions are master's, as check_tag fetched it, like the allow-list: a finding the
    # owner accepts on master after the tag counts when the job is retried.
    exceptions, missing = master_exceptions(root, run)
    if missing:
        problems.append(missing)
    problems.extend(security_gate.evaluate(root, root, exceptions_text=exceptions or "exceptions: []\n", say=say))

    places = sources(version)
    for name in collected_assets(version):
        path = os.path.join(root, places[name])
        if not os.path.isfile(path) or os.path.getsize(path) == 0:
            problems.append("%s is not in this job's artifacts (%s): the job that makes it failed or did not run" % (name, places[name]))
    if not problems:
        names = feed.installer_names(version)
        problems.extend(check_sums(os.path.join(root, places[sums_name(version)]),
                                   os.path.join(root, places[names["deb-amd64"]]),
                                   os.path.join(root, places[names["rpm-x86_64"]])))
        problems.extend(sbom_problems(os.path.join(root, "linux-build", "packages"), version,
                                      [names["deb-amd64"], names["rpm-x86_64"]]))
    if not problems:
        problems.extend(check_registry_before_signing(api, version, dict(
            (name, sha256_of(os.path.join(root, places[name]))) for name in collected_assets(version))))
    if problems:
        return problems

    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    for name in collected_assets(version):
        shutil.copyfile(os.path.join(root, places[name]), os.path.join(out, name))
        say("  %s  %12d  %s" % (sha256_of(os.path.join(out, name)), os.path.getsize(os.path.join(out, name)), name))
    return []


# ---------------------------------------------------------------- GitLab

class GitLab(object):
    """The API as the job's own token: JOB-TOKEN, sent as a header, never printed."""

    def __init__(self, token):
        self.token = token

    def request(self, method, url, body=None, content_type=None):
        headers = {"User-Agent": "hmailserver-release-chain", "Accept": "application/json"}
        if self.token:
            headers["JOB-TOKEN"] = self.token
        if content_type:
            headers["Content-Type"] = content_type
        request = urllib.request.Request(url, data=body, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=600) as response:
                return response.status, response.read(), dict((k.lower(), v) for k, v in response.headers.items())
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict((k.lower(), v) for k, v in e.headers.items())
        except (urllib.error.URLError, OSError) as e:
            raise ReleaseError("%s %s could not be reached: %s" % (method, url.split("?")[0], e))

    def feed_get(self, url):
        """build/generate-update-feed.py's get: (body, headers), or FeedError."""
        status, body, headers = self.request("GET", url)
        if status != 200:
            raise feed.FeedError("%s answered HTTP %d" % (url.split("?")[0], status))
        return body, headers


def message_of(body):
    try:
        value = json.loads(body.decode("utf-8"))
        return json.dumps(value.get("message") or value.get("error") or value)[:300] if isinstance(value, dict) else str(value)[:300]
    except (ValueError, UnicodeDecodeError):
        return body[:200].decode("utf-8", "replace")


def registry_files(api, version):
    """Every record the registry holds of each file of hmailserver/<version>, oldest
    first. A generic package can hold a name more than once, and a download by name
    is served the newest, so each is read."""
    packages = feed.get_all(api.feed_get, "/packages?package_type=generic&package_name=%s" % PACKAGE)
    held = {}
    for package in packages:
        if package.get("name") != PACKAGE or package.get("version") != version:
            continue
        for entry in feed.get_all(api.feed_get, "/packages/%d/package_files" % package["id"]):
            held.setdefault(entry.get("file_name"), []).append(entry)
    for entries in held.values():
        entries.sort(key=lambda e: e.get("id", 0))
    return held


def registry_conflicts(held, digests, version):
    """The files of digests ({name: sha256}) the registry already holds with other
    bytes, as problems, and the names it holds with these bytes and no other."""
    problems = []
    same = set()
    for name in sorted(digests):
        entries = held.get(name) or []
        other = [e for e in entries if (e.get("file_sha256") or "").lower() != digests[name]]
        if other:
            problems.append("the package %s/%s already holds %s with the SHA-256 %s, not %s, this release's: a file name "
                            "is a release's once, and the link a server downloads by name is served the newest. Nothing "
                            "has been uploaded (RELEASE.md, 'A tag is spent once')" % (
                                PACKAGE, version, name, other[0].get("file_sha256") or "unknown", digests[name]))
        elif entries:
            same.add(name)
    return problems, same


def check_registry_before_signing(api, version, digests):
    """collect's look at the registry. The .deb, the .rpm, the sums and the SBOMs are
    final here, and the registry may hold them only with these bytes, as it does
    after a release-publish that stopped part-way. The installer and every bundle are
    made by the jobs after this one (Authenticode, Sigstore), and a second signing
    never makes the same bytes, so the registry may not hold them at all: such a
    release is finished by retrying release-publish of the pipeline that signed them."""
    if api is None:
        return ["there is no CI_JOB_TOKEN to read the package registry with: collect runs in the release job and nowhere else"]
    try:
        held = registry_files(api, version)
    except (ReleaseError, feed.FeedError) as e:
        return ["the package registry could not be read, and a release is not signed without knowing what it holds "
                "under %s/%s: %s" % (PACKAGE, version, e)]
    later = made_after_collect(version)
    final = dict((name, digest) for name, digest in digests.items() if name not in later)
    problems, _ = registry_conflicts(held, final, version)
    for name in later + sorted(n + ".cosign.bundle" for n in set(digests) | set(later)):
        if held.get(name):
            problems.append("the package %s/%s already holds %s, which an earlier run's signing made: signing again makes "
                            "other bytes, which release-publish refuses. Retry release-publish in the pipeline that signed "
                            "it, or, if that cannot be, the tag is spent (RELEASE.md, 'A tag is spent once')" % (
                                PACKAGE, version, name))
    return problems


def make_installer_sbom(root, tag, env, assets, installer_path, dependencies_path, out):
    """release-sbom: the installer's own SBOM, from the bytes Authenticode signed."""
    version = version_of(tag)
    installer = feed.installer_names(version)["windows"]
    if os.path.basename(installer_path) != installer or not os.path.isfile(installer_path):
        raise ReleaseError("%s is not the signed installer %s: release-authenticode's artifact did not arrive" % (installer_path, installer))
    unsigned = os.path.join(assets, installer)
    if os.path.isfile(unsigned) and sha256_of(unsigned) == sha256_of(installer_path):
        raise ReleaseError("%s is byte for byte the installer collect gathered: Authenticode signed nothing, and the SBOM "
                           "would describe bytes that do not ship" % installer_path)
    try:
        with open(dependencies_path, encoding="utf-8-sig") as f:
            dependencies = json.load(f)
        document = asset_sbom.make("windows-x64", installer_path, dependencies, root=root, version=version,
                                   commit=env.get("CI_COMMIT_SHA", ""))
    except (OSError, ValueError, asset_sbom.SbomError) as e:
        raise ReleaseError("the installer's SBOM could not be made: %s" % e)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    path = os.path.join(out, sbom_name(installer))
    asset_sbom.write_document(document, path)
    say("  %s  %d components, describing %s (SHA-256 %s)" % (sbom_name(installer), len(document["components"]), installer,
                                                            sha256_of(installer_path)))
    return path


def create_release(api, payload, version):
    """POSTs the release in the milestone of its version, so the release page lists
    the issues closed for it, and again without one when GitLab refuses the
    milestone, as it does when the project has none of that title. The title is
    named in the request, not looked up first: a job's token may use the Releases
    API, which finds the milestone as the job's user, and not the Milestones API
    (GitLab's job token documentation lists the APIs it may use). Never a reason to
    stop: the release matters, the link does not. Leaves payload as it was last
    sent, and returns GitLab's status and body."""
    payload["milestones"] = [version]
    status, body, _ = api.request("POST", feed.API + "/releases", json.dumps(payload).encode("utf-8"), "application/json")
    if status in (400, 422) and "milestone" in message_of(body).lower():
        say("no milestone linked: creating the release in the milestone %s answered HTTP %d: %s. Creating it without one"
            % (version, status, message_of(body)))
        del payload["milestones"]
        status, body, _ = api.request("POST", feed.API + "/releases", json.dumps(payload).encode("utf-8"), "application/json")
    return status, body


def publish(assets, tag, env, api, notes, verification, render_check=True):
    """Uploads, reads back, creates the release, and checks the feed takes it."""
    version = version_of(tag)
    if env.get("CI_PROJECT_ID") != PROJECT_ID:
        raise ReleaseError("this is project %s, not %s: a release is published from the project the servers trust "
                           "and nowhere else" % (env.get("CI_PROJECT_ID"), PROJECT_ID))
    if (env.get("CI_API_V4_URL") or "").rstrip("/") != API_BASE:
        raise ReleaseError("the API is %s, not %s, which every release link names" % (env.get("CI_API_V4_URL"), API_BASE))

    expected = expected_assets(version)
    wanted = set(expected) | set(n + ".cosign.bundle" for n in expected)
    present = set(n for n in os.listdir(assets) if os.path.isfile(os.path.join(assets, n)))
    if present != wanted:
        parts = []
        if wanted - present:
            parts.append("missing " + ", ".join(sorted(wanted - present)))
        if present - wanted:
            parts.append("extra " + ", ".join(sorted(present - wanted)))
        raise ReleaseError("%s is not a release (%s): a release is exactly %s, and a bundle beside each" % (
            assets, "; ".join(parts), ", ".join(expected)))

    unfit = sbom_problems(assets, version)
    if unfit:
        raise ReleaseError("; ".join(unfit) + ". Nothing was uploaded")

    status, body, _ = api.request("GET", "%s/releases/%s" % (feed.API, urllib.parse.quote(tag, safe="")))
    if status == 200:
        raise ReleaseError("the release %s exists already. A tag is spent once: nothing was uploaded, and nothing on "
                           "it was changed" % tag)
    if status != 404:
        raise ReleaseError("asking whether %s exists answered HTTP %d: %s" % (tag, status, message_of(body)))

    digests = dict((name, sha256_of(os.path.join(assets, name))) for name in wanted)
    # A name the package holds with other bytes is refused before anything is sent;
    # one it holds with these bytes, from a run of this job that stopped part-way,
    # is not sent again.
    conflicts, same = registry_conflicts(registry_files(api, version), digests, version)
    if conflicts:
        raise ReleaseError("; ".join(conflicts))
    for name in sorted(wanted):
        if name in same:
            say("  %s is in the registry with these bytes already, from an earlier run: not uploaded again" % name)
            continue
        with open(os.path.join(assets, name), "rb") as f:
            data = f.read()
        status, body, _ = api.request("PUT", registry_url(version, name) + "?select=package_file", data, "application/octet-stream")
        if status not in (200, 201):
            raise ReleaseError("uploading %s answered HTTP %d: %s. Nothing is released until every file is up; retry "
                               "this job, which does not send again what is up already" % (name, status, message_of(body)))
        say("  uploaded %s (%d bytes)" % (name, len(data)))

    # Every record of each name, since a download by name is served the newest.
    held = registry_files(api, version)
    for name in sorted(wanted):
        entries = held.get(name) or []
        if not entries:
            raise ReleaseError("the registry has no %s in %s/%s after its upload" % (name, PACKAGE, version))
        for entry in entries:
            if (entry.get("file_sha256") or "").lower() != digests[name]:
                raise ReleaseError("the registry's %s has the SHA-256 %s, not %s, the file that was signed" % (
                    name, entry.get("file_sha256"), digests[name]))
    say("the registry holds all %d files with the digests that were signed, and no other" % len(wanted))

    description = "# hMailServer %s\n\n%s\n\n%s\n" % (version, notes.strip(), verification.strip())
    payload = {"tag_name": tag, "name": "hMailServer %s" % version, "description": description,
               "assets": {"links": release_links(version, wanted)}}
    status, body = create_release(api, payload, version)
    if status not in (200, 201):
        raise ReleaseError("creating the release %s answered HTTP %d: %s. The files are in the registry; run the job "
                           "again once the cause is fixed" % (tag, status, message_of(body)))
    say("created the release %s with %d links%s" % (tag, len(wanted), ", in the milestone " + version if "milestones" in payload else ""))

    if render_check:
        try:
            check_feed_takes(api, tag, version)
        except (ReleaseError, feed.FeedError) as e:
            raise ReleaseError("the release %s is created, but %s. Servers are not offered it until that is put right: "
                               "read the release's links against docs/UpdateFeed.md, 'What the site serves'" % (tag, e))
    return payload


def check_feed_takes(api, tag, version):
    """The feed the pages job will render includes this release, and a Windows, a
    Debian and an RPM server each find its file and bundle in it."""
    releases, files = feed.load(api.feed_get)
    tree, latest, warnings = feed.render(releases, files)
    ours = [w for w in warnings if w.startswith(tag + " ") or (tag + ":") in w]
    key = "releases/tags/" + tag
    if key not in tree or ours:
        raise ReleaseError("the feed leaves %s out: %s" % (tag, "; ".join(ours) or "it is not in the render"))
    document = json.loads(tree[key].decode("utf-8"))
    for platform in ("windows", "deb-amd64", "rpm-x86_64"):
        release = feed.read_release(document, platform)
        if not release or not release["installer_url"] or not release["bundle_url"] or not release["installer_digest"]:
            raise ReleaseError("a %s server would find no file with its bundle in %s" % (platform, tag))
    say("the feed takes %s: Windows, Debian and RPM servers each find their file and its bundle; releases/latest is %s"
          % (tag, latest["tag_name"]))


# ---------------------------------------------------------------- the self-test

def self_test():
    QUIET.append(True)
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    version = "6.3.5"
    tag = "v6.3.5"
    names = feed.installer_names(version)

    # The names and links, held to the feed's own rules.
    expect(expected_assets(version)[0] == "hMailServer-6.3.5-x64.exe", "the installer is named as a Windows server looks for it")
    for name in expected_assets(version):
        for file_name in (name, name + ".cosign.bundle"):
            match = feed.REGISTRY_LINK.match(registry_url(version, file_name))
            expect(match is not None and urllib.parse.unquote(match.group(3)) == file_name
                   and (urllib.parse.unquote(match.group(1)), urllib.parse.unquote(match.group(2))) == ("hmailserver", version),
                   "the link of %s is a file of hmailserver %s that the feed accepts" % (file_name, version))
    expect(permalink_name("hMailServer-6.3.5-x64.exe", version) == "hMailServer-x64.exe", "the installer's permanent name")
    expect(permalink_name("hmailserver_6.3.5_amd64.deb", version) == "hmailserver_amd64.deb", "the .deb's permanent name")
    expect(permalink_name("hmailserver-6.3.5-1.x86_64.rpm", version) == "hmailserver.x86_64.rpm", "the .rpm's permanent name")
    expect(permalink_name("hmailserver-linux-6.3.5-SHA256SUMS.txt", version) == "hmailserver-linux-SHA256SUMS.txt", "the sums' permanent name")
    expect(permalink_name("hMailServer-6.3.5-x64.exe.cosign.bundle", version) == "hMailServer-6.3.5-x64.exe.cosign.bundle", "a bundle keeps its name")
    expect(permalink_name("hmailserver.spdx.json", version) == "hmailserver.spdx.json", "an SBOM keeps its name")
    expect(link_type("hmailserver_6.3.5_amd64.deb") == "package" and link_type("hmailserver.spdx.json") == "other"
           and link_type("hMailServer-6.3.5-x64.exe.cosign.bundle") == "other", "packages are packages, the rest is other")
    for bad in ("6.3.5", "v", "build-inputs-1", "vx.1"):
        try:
            version_of(bad)
            failures.append("%r is refused as a release tag" % bad)
        except ReleaseError:
            pass
    expect(version_of("v6.3.5-rc1") == "6.3.5-rc1", "a pre-release's tag names its version")

    changelog = ("# Changelog\n\n## Unreleased - in the tree after 6.3.5\n\nNothing yet.\n\n---\n\n"
                 "## [6.3.5](https://gitlab.com/x/-/releases/v6.3.5) - 2026-10-10\n\nThe notes.\n\n### A part\n\nMore.\n\n---\n\n"
                 "## [6.3.4](https://gitlab.com/x/-/releases/v6.3.4) - 2026-09-26\n\nOlder.\n")
    section = changelog_section(changelog, "6.3.5")
    expect(section.startswith("The notes.") and "### A part" in section and "Older." not in section, "the version's section, and no other: %r" % section)
    try:
        changelog_section(changelog, "6.3.6")
        failures.append("a version with no section in CHANGELOG.md is refused")
    except ReleaseError:
        pass

    # The allow-list's keys and their fingerprints. KEY_A is the owner's, from
    # .gitlab/allowed_signers, with the fingerprint ssh-keygen -l prints for it.
    key_a = "AAAAC3NzaC1lZDI1NTE5AAAAIIB9HiI7F4JgXDg9DyvL3KSQUlKfygNjVkT2rKf6pOUL"
    key_b = base64.b64encode(b"\x00\x00\x00\x0bssh-ed25519\x00\x00\x00\x20" + bytes(range(32))).decode("ascii")
    list_a = "# the owner's\nowner@example.org ssh-ed25519 %s\n" % key_a
    list_b = "someone@example.org ssh-ed25519 %s\n" % key_b
    expect(fingerprint(key_a) == "SHA256:bz6MOtUcFMhhzlNiUhIZL0BzitPE875hMk1Yon5huAA", "a key's fingerprint is ssh-keygen's")
    expect(fingerprint("not base64!") == "" and fingerprint("") == "", "a key that is missing or not base64 has no fingerprint")
    expect(signer_keys('# c\n\na@b.c,d@e.f namespaces="git" ssh-ed25519 %s comment\n' % key_a) == [("a@b.c,d@e.f", "ssh-ed25519", key_a)],
           "an allowed_signers line with options is read as its principals and its key")
    expect(len(signer_keys(list_a + list_b)) == 2 and signer_keys("# only a comment\n") == [], "every key line is counted, no comment")

    with tempfile.TemporaryDirectory() as work:
        # A checkout with the jobs' artifacts in it, as `release` sees one.
        root = os.path.join(work, "checkout")

        def write(relative, data):
            path = os.path.join(root, relative)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            return path

        write(VERSION_H, b'#pragma once\n#define HMAILSERVER_VERSION "6.3.5"\n#define HMAILSERVER_VERSION_NUMERIC 6,3,5,44\n')
        for name, relative in sources(version).items():
            write(relative, ("the bytes of %s\n" % name).encode("utf-8"))

        def sbom_of(path, kind):
            """A document this project's build/asset-sbom.py would accept as describing path."""
            sha256, sha512, size = asset_sbom.digests(path)
            subject = asset_sbom.subject(os.path.basename(path), kind, version, sha256, sha512, size, "c" * 40)
            return json.dumps(asset_sbom.document(subject, [], sha256, kind)).encode("utf-8")

        for key, kind in (("deb-amd64", "deb"), ("rpm-x86_64", "rpm")):
            write(sources(version)[sbom_name(names[key])], sbom_of(os.path.join(root, sources(version)[names[key]]), kind))
        deb = os.path.join(root, sources(version)[names["deb-amd64"]])
        rpm = os.path.join(root, sources(version)[names["rpm-x86_64"]])
        sums = "%s  %s\n%s  %s\n" % (sha256_of(deb), names["deb-amd64"], sha256_of(rpm), names["rpm-x86_64"])
        write(sources(version)[sums_name(version)], sums.encode("utf-8"))
        # The tagged tree's own allow-list names another key than master's: a tag
        # signed with it must be refused, which it is only if master's is read.
        write(ALLOWED_SIGNERS, list_b.encode("utf-8"))
        # The four scans' artifacts as a pipeline that found nothing leaves them, and an
        # exceptions file that accepts nothing.
        for relative, content in security_gate.sample_reports().items():
            write(relative, content.encode("utf-8"))
        write(security_gate.EXCEPTIONS, b"exceptions: []\n")

        built = "c" * 40
        env = {"HM_COSIGN_IMAGE": "gcr.io/projectsigstore/cosign:v2.6.1-dev@sha256:" + "a" * 64, "CI_COMMIT_SHA": built}
        for name in AZURE_SETTINGS:
            env[name] = "set"

        class Done(object):
            def __init__(self, code, out="", err=""):
                self.returncode, self.stdout, self.stderr = code, out, err

        def fake_git(commit=built, on_master=True, master_list=list_a, signed_by=key_a, fetch_master=True, names_key=None,
                     hidden=False, unshallow=True, master_exceptions="exceptions: []\n"):
            """git as the release job's checkout answers it. master's history and its
            allow-list are there only once master has been fetched by name; with
            hidden, another shallow boundary hides the way down to the tagged commit
            until master's whole history is fetched. verify-tag reads whatever
            allow-list it is given, and a signature is good when that list holds the
            key the tag was signed with."""
            state = {"master": False, "whole": False}

            def answer(arguments, **kwargs):
                if "cat-file" in arguments:
                    return Done(0, "tag\n")
                if "--is-shallow-repository" in arguments:
                    return Done(0, "false\n" if state["whole"] else "true\n")
                if "rev-parse" in arguments:
                    return Done(0, commit + "\n")
                if "fetch" in arguments:
                    if "+refs/heads/%s:%s" % (TRUSTED_BRANCH, TRUSTED_REF) in arguments:
                        if not fetch_master or ("--unshallow" in arguments and not unshallow):
                            return Done(128, "", "fatal: could not read from remote repository")
                        state["master"] = True
                        state["whole"] = state["whole"] or "--unshallow" in arguments
                    return Done(0)
                if "merge-base" in arguments:
                    seen = state["master"] and (state["whole"] or not hidden)
                    return Done(0 if on_master and seen and TRUSTED_REF in arguments else 1)
                if "show" in arguments:
                    if state["master"] and arguments[-1] == "%s:%s" % (TRUSTED_REF, ALLOWED_SIGNERS) and master_list is not None:
                        return Done(0, master_list)
                    if state["master"] and arguments[-1] == "%s:%s" % (TRUSTED_REF, security_gate.EXCEPTIONS) and master_exceptions is not None:
                        return Done(0, master_exceptions)
                    return Done(128, "", "fatal: path '%s' does not exist" % ALLOWED_SIGNERS)
                if "verify-tag" in arguments:
                    given = [a.split("=", 1)[1] for a in arguments if a.startswith("gpg.ssh.allowedSignersFile=")]
                    path = given[0] if given else ""
                    path = path if os.path.isabs(path) else os.path.join(root, path)
                    try:
                        with open(path, encoding="utf-8") as f:
                            listed = signer_keys(f.read())
                    except OSError:
                        return Done(1, "", "error: no allowed signers file")
                    for principal, _, key in listed:
                        if key == signed_by:
                            shown = names_key or key
                            return Done(0, "", 'Good "git" signature for %s with ED25519 key %s\n' % (principal, fingerprint(shown)))
                    return Done(1, "", "Could not verify signature.\nno principal matched")
                return Done(0)
            return answer

        class FakeGitLab(object):
            def __init__(self):
                self.files = {}
                self.releases = []
                self.calls = []
                self.next_id = [100]
                self.milestones = []
                self.posted = []

            def seed(self, name, data):
                """A file the registry holds before this run, as an earlier one or someone else left it."""
                self.next_id[0] += 1
                self.files.setdefault((PACKAGE, version), []).append({"id": self.next_id[0], "file_name": name, "size": len(data),
                                                                      "file_sha256": hashlib.sha256(data).hexdigest()})
                return self

            def request(self, method, url, body=None, content_type=None):
                self.calls.append((method, url.split("?")[0]))
                parsed = urllib.parse.urlparse(url)
                path = parsed.path[len("/api/v4/projects/" + PROJECT_ID):]
                query = urllib.parse.parse_qs(parsed.query)
                if method == "PUT":
                    m = re.match(r"^/packages/generic/([^/]+)/([^/]+)/([^/]+)$", path)
                    self.next_id[0] += 1
                    key = (urllib.parse.unquote(m.group(1)), urllib.parse.unquote(m.group(2)))
                    self.files.setdefault(key, []).append({"id": self.next_id[0], "file_name": urllib.parse.unquote(m.group(3)),
                                                           "size": len(body), "file_sha256": hashlib.sha256(body).hexdigest()})
                    return 201, b"{}", {}
                if method == "POST" and path == "/releases":
                    value = json.loads(body.decode("utf-8"))
                    self.posted.append(value)
                    # As GitLab's Releases API: a milestone is named by its exact
                    # title, and one the project does not have refuses the release.
                    missing = [t for t in value.get("milestones", []) if t not in self.milestones]
                    if missing:
                        return 400, json.dumps({"message": "Milestone(s) not found: " + ", ".join(missing)}).encode("utf-8"), {}
                    self.releases.insert(0, {"tag_name": value["tag_name"], "name": value["name"], "upcoming_release": False,
                                             "released_at": "2026-10-10T10:00:00.000Z", "description": value["description"],
                                             "_links": {"self": feed.PROJECT_URL + "/-/releases/" + value["tag_name"]},
                                             "assets": {"links": value["assets"]["links"]}})
                    return 201, b"{}", {}
                if method == "GET":
                    m = re.match(r"^/releases/([^/]+)$", path)
                    if m:
                        found = [r for r in self.releases if r["tag_name"] == urllib.parse.unquote(m.group(1))]
                        return (200, json.dumps(found[0]).encode("utf-8"), {}) if found else (404, b'{"message":"404 Not Found"}', {})
                    if path == "/milestones":
                        # A job's token may not use the Milestones API.
                        return 401, b'{"message":"401 Unauthorized"}', {}
                    page = int(query.get("page", ["1"])[0])
                    if path == "/releases":
                        items = self.releases
                    elif path == "/packages":
                        items = [{"id": i, "name": k[0], "version": k[1]} for i, k in enumerate(sorted(self.files))]
                    else:
                        m = re.match(r"^/packages/(\d+)/package_files$", path)
                        items = self.files[sorted(self.files)[int(m.group(1))]]
                    return 200, json.dumps(items[(page - 1) * 100:page * 100]).encode("utf-8"), {"x-next-page": ""}
                return 400, b'{"message":"unexpected"}', {}

            def feed_get(self, url):
                status, body, headers = self.request("GET", url)
                if status != 200:
                    raise feed.FeedError("HTTP %d" % status)
                return body, headers

        signed_tag = fake_git()

        out = os.path.join(work, "release-assets")
        problems = collect(root, tag, out, env, run=signed_tag, api=FakeGitLab())
        expect(problems == [], "a complete release, its tag on master and signed by master's one key, is collected: %s" % problems)
        gathered = sorted(os.listdir(out)) if os.path.isdir(out) else []
        expect(gathered == sorted(collected_assets(version)), "under the names a release carries: %s" % (gathered or "nothing"))
        expect(sorted(set(expected_assets(version)) - set(collected_assets(version))) == [sbom_name(names["windows"])],
               "everything but the installer's SBOM, which release-sbom makes from the signed installer")
        expect(all(sbom_name(n) in expected_assets(version) for n in compiled_assets(version)),
               "every compiled file goes out with its SBOM")
        if not gathered:
            # So that publishing is still tested when collecting failed.
            os.makedirs(out, exist_ok=True)
            for name in collected_assets(version):
                shutil.copyfile(os.path.join(root, sources(version)[name]), os.path.join(out, name))

        def refused(what, needle, **changes):
            e = dict(env)
            e.update(changes.get("env", {}))
            run = changes.get("run", signed_tag)
            api = changes["api"] if "api" in changes else FakeGitLab()
            found = collect(root, changes.get("tag", tag), os.path.join(work, "refused"), e, run=run, api=api)
            expect(any(needle in p for p in found), "%s is refused: %s" % (what, found))
            expect(not os.path.isdir(os.path.join(work, "refused")), "%s copies nothing" % what)

        refused("a tag Version.h does not name", "Version.h says 6.3.5", tag="v6.3.6")
        refused("a cosign image left unpinned", "HM_COSIGN_IMAGE", env={"HM_COSIGN_IMAGE": "gcr.io/projectsigstore/cosign:v2.6.1-dev@sha256:PIN-THE-DIGEST"})
        refused("a cosign image from ghcr.io, pinned, as a CI/CD variable could set it", "not gcr.io/projectsigstore/cosign",
                env={"HM_COSIGN_IMAGE": "ghcr.io/sigstore/cosign/cosign:v2.6.1-dev@sha256:" + "a" * 64})
        refused("a tag moved to another commit after the pipeline started", "the tag was moved", run=fake_git(commit="d" * 40))
        refused("a collect outside a pipeline", "CI_COMMIT_SHA is", env={"CI_COMMIT_SHA": ""})
        refused("an Azure setting left out", "ARTIFACT_SIGNING_PROFILE", env={"ARTIFACT_SIGNING_PROFILE": " "})
        refused("a lightweight tag", "is not an annotated tag", run=lambda a, **k: Done(0, "commit\n"))
        # The tag's signer, against master's allow-list and not the tagged tree's.
        refused("a tag on a commit that is not on master", "which is not on master", run=fake_git(on_master=False))
        refused("a tag not on master, asked again with master's whole history", "which is not on master",
                run=fake_git(on_master=False, hidden=True))
        refused("a checkout that cannot fetch master's whole history when a shallow boundary hides the way down",
                "whole history could not be fetched", run=fake_git(hidden=True, unshallow=False))
        problems = collect(root, tag, os.path.join(work, "deepened"), env, run=fake_git(hidden=True), api=FakeGitLab())
        expect(problems == [], "a tag on master whose way down a shallow boundary hides is found with master's whole history: %s" % problems)
        refused("a tag signed by the key its own tree lists and master does not", "is not signed by the key master's",
                run=fake_git(signed_by=key_b))
        refused("a tag signed by a second key master's list gained", "lists 2 key(s)",
                run=fake_git(master_list=list_a + list_b, signed_by=key_b))
        refused("master's list of two keys, even for the owner's", "lists 2 key(s)", run=fake_git(master_list=list_a + list_b))
        refused("master's list with no key", "lists 0 key(s)", run=fake_git(master_list="# nobody\n"))
        refused("no allow-list on master", "master has no .gitlab/allowed_signers", run=fake_git(master_list=None))
        refused("master not fetched", "master could not be fetched", run=fake_git(fetch_master=False))
        refused("git naming another key than the listed one", "not by the one key master's", run=fake_git(names_key=key_b))
        # The registry, before anything is signed.
        refused("a collect without the job's token", "there is no CI_JOB_TOKEN", api=None)
        refused("a .deb the registry holds with other bytes", "already holds %s with the SHA-256" % names["deb-amd64"],
                api=FakeGitLab().seed(names["deb-amd64"], b"other bytes"))
        refused("an installer an earlier run's signing left in the registry", "already holds %s, which an earlier run's signing made" % names["windows"],
                api=FakeGitLab().seed(names["windows"], ("the bytes of %s\n" % names["windows"]).encode("utf-8")))
        refused("a bundle an earlier run left in the registry", "hmailserver.spdx.json.cosign.bundle, which an earlier run's signing made",
                api=FakeGitLab().seed("hmailserver.spdx.json.cosign.bundle", b"{}"))
        refused("an installer SBOM an earlier run left in the registry", "%s, which an earlier run's signing made" % sbom_name(names["windows"]),
                api=FakeGitLab().seed(sbom_name(names["windows"]), b"{}"))
        held = FakeGitLab()
        for name in collected_assets(version):
            if name != names["windows"]:
                with open(os.path.join(root, sources(version)[name]), "rb") as f:
                    held.seed(name, f.read())
        problems = collect(root, tag, os.path.join(work, "again"), env, run=signed_tag, api=held)
        expect(problems == [], "files the registry holds with the same bytes, from a publish that stopped part-way, are collected again: %s" % problems)
        write(sources(version)[sums_name(version)], ("%s  %s\n" % ("0" * 64, names["deb-amd64"])).encode("utf-8"))
        refused("a sums file that is not the packages'", "is not the sums of the two packages")
        write(sources(version)[sums_name(version)], sums.encode("utf-8"))
        deb_sbom = os.path.join(root, sources(version)[sbom_name(names["deb-amd64"])])
        with open(deb_sbom, "rb") as f:
            good_sbom = f.read()
        write(sources(version)[sbom_name(names["deb-amd64"])], sbom_of(os.path.join(root, sources(version)[names["rpm-x86_64"]]), "deb"))
        refused("a .deb whose SBOM describes another file", "%s does not describe %s" % (sbom_name(names["deb-amd64"]), names["deb-amd64"]))
        write(sources(version)[sbom_name(names["deb-amd64"])], b"not json")
        refused("a .deb whose SBOM is not JSON", "%s does not describe" % sbom_name(names["deb-amd64"]))
        os.remove(deb_sbom)
        refused("a .deb without its SBOM", "%s is not in this job's artifacts" % sbom_name(names["deb-amd64"]))
        write(sources(version)[sbom_name(names["deb-amd64"])], good_sbom)
        os.remove(os.path.join(root, sources(version)["hmailserver.spdx.json"]))
        refused("a missing SBOM", "hmailserver.spdx.json is not in this job's artifacts")
        write(sources(version)["hmailserver.spdx.json"], b"the bytes of hmailserver.spdx.json\n")
        # The security scans: a report missing, or a scan that failed, holds the release.
        samples = security_gate.sample_reports()
        os.remove(os.path.join(root, security_gate.GITLAB_SAST_REPORT))
        refused("a release whose semgrep-sast report is missing", "semgrep-sast: there is no")
        write(security_gate.GITLAB_SAST_REPORT, samples[security_gate.GITLAB_SAST_REPORT].encode("utf-8"))
        failed = json.loads(samples[security_gate.DEPENDENCY_REPORTS[1]])
        failed["scan"]["status"] = "failure"
        write(security_gate.DEPENDENCY_REPORTS[1], json.dumps(failed).encode("utf-8"))
        refused("a release whose dependency scan could not run", "not success")
        write(security_gate.DEPENDENCY_REPORTS[1], samples[security_gate.DEPENDENCY_REPORTS[1]].encode("utf-8"))
        refused("a release with an exceptions file the gate cannot read", "it has no finding",
                run=fake_git(master_exceptions="exceptions:\n  - scan: semgrep\n"))
        refused("master without an exceptions file", "master's .gitlab/security-exceptions.yml could not be read",
                run=fake_git(master_exceptions=None))
        # A finding that holds the release, accepted by the tagged tree's own copy and not
        # by master's: master's is the one read.
        advisory = json.loads(samples[security_gate.DEPENDENCY_REPORTS[0]])
        advisory["vulnerabilities"] = [{"id": "u", "name": "CVE-2026-0001 in zlib 1.3", "severity": "Critical",
                                        "identifiers": [{"type": "cve", "name": "CVE-2026-0001", "value": "CVE-2026-0001"}],
                                        "location": {"file": "lock", "dependency": {"package": {"name": "zlib"}, "version": "1.3"}},
                                        "solution": "Upgrade zlib to 1.3.1."}]
        write(security_gate.DEPENDENCY_REPORTS[0], json.dumps(advisory).encode("utf-8"))
        # Reviewed today (UTC, as the gate judges), in force for half a year.
        clock = security_gate.datetime
        reviewed = clock.datetime.now(clock.timezone.utc).date()
        accepted = ("exceptions:\n  - scan: dependency-scan\n    finding: CVE-2026-0001\n    package: zlib\n    what: zlib\n"
                    "    reason: 'the vulnerable inflate path is not reached: the server only deflates'\n"
                    "    reviewed-by: Progressiverobot\n    reviewed: %s\n    expires: %s\n" % (
                        reviewed.isoformat(), (reviewed + clock.timedelta(days=180)).isoformat()))
        write(security_gate.EXCEPTIONS, accepted.encode("utf-8"))
        refused("a finding the tagged tree accepts and master does not", "CVE-2026-0001 in zlib")
        write(security_gate.EXCEPTIONS, b"exceptions: []\n")
        problems = collect(root, tag, os.path.join(work, "accepted"), env, run=fake_git(master_exceptions=accepted), api=FakeGitLab())
        expect(problems == [], "a finding master's exceptions accept is collected: %s" % problems)
        write(security_gate.DEPENDENCY_REPORTS[0], samples[security_gate.DEPENDENCY_REPORTS[0]].encode("utf-8"))
        problems = collect(root, tag, os.path.join(work, "scanned"), env, run=signed_tag, api=FakeGitLab())
        expect(problems == [], "the scans put back, the release is collected again: %s" % problems)

        # release-sbom: the installer's SBOM is made from the signed bytes, never the unsigned.
        signed_dir = os.path.join(work, "release-authenticode")
        os.makedirs(signed_dir)
        shutil.copyfile(os.path.join(out, names["windows"]), os.path.join(signed_dir, names["windows"]))
        try:
            make_installer_sbom(root, tag, env, out, os.path.join(signed_dir, names["windows"]), os.path.join(work, "none.json"),
                                os.path.join(work, "release-sbom"))
            failures.append("an installer Authenticode left unchanged gets no SBOM")
        except ReleaseError as e:
            expect("signed nothing" in str(e), "an installer Authenticode left unchanged gets no SBOM, saying why: %s" % e)
        with open(os.path.join(signed_dir, names["windows"]), "ab") as f:
            f.write(b"a signature")
        try:
            make_installer_sbom(root, tag, env, out, os.path.join(signed_dir, names["windows"]), os.path.join(work, "none.json"),
                                os.path.join(work, "release-sbom"))
            failures.append("an installer SBOM without the component list is refused")
        except ReleaseError as e:
            expect("could not be made" in str(e), "an installer SBOM without the component list is refused, saying why: %s" % e)
        expect(not os.path.isdir(os.path.join(work, "release-sbom")), "and nothing is written")

        # Publishing, against a registry and a releases API held in memory. The
        # installer's SBOM, as release-sbom makes it, describes the installer.
        signed = os.path.join(work, "release-signed")
        shutil.copytree(out, signed)
        with open(os.path.join(signed, sbom_name(names["windows"])), "wb") as f:
            f.write(sbom_of(os.path.join(signed, names["windows"]), "windows-x64"))
        for name in expected_assets(version):
            with open(os.path.join(signed, name + ".cosign.bundle"), "w") as f:
                f.write('{"mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json"}')

        publish_env = {"CI_PROJECT_ID": PROJECT_ID, "CI_API_V4_URL": API_BASE + "/"}
        verification = "## Verification\n\nSigned by %s%s." % (SIGNER_IDENTITY_PREFIX, tag)
        api = FakeGitLab()
        payload = None
        try:
            payload = publish(signed, tag, publish_env, api, section, verification)
        except ReleaseError as e:
            failures.append("a complete signed release is published: %s" % e)
        if payload:
            links = payload["assets"]["links"]
            expect(len(links) == 18 and all(l["name"] == urllib.parse.unquote(feed.REGISTRY_LINK.match(l["url"]).group(3)) for l in links),
                   "eighteen links, each named after the file it points at")
            expect(len(set(l["direct_asset_path"] for l in links)) == 18, "every permanent download path is its own")
            expect(payload["name"] == "hMailServer 6.3.5" and "The notes." in payload["description"] and SIGNER_IDENTITY_PREFIX in payload["description"],
                   "the notes are the CHANGELOG section and the verification")
            uploads = [c for c in api.calls if c[0] == "PUT"]
            created = [i for i, c in enumerate(api.calls) if c[0] == "POST"]
            expect(len(uploads) == 18 and created and all(api.calls.index(u) < created[0] for u in uploads),
                   "every file is uploaded before the release links to it")
            # And the feed, rendered from what was published, gives each kind of server its file.
            tree, latest, warnings = feed.render(*feed.load(api.feed_get))
            expect("releases/tags/v6.3.5" in tree and latest["tag_name"] == "v6.3.5" and not warnings,
                   "the feed publishes the release as the stable one: %s" % warnings)
            expect("milestones" not in payload, "a release is created without a milestone when the project has none of its name")
            expect([p.get("milestones") for p in api.posted] == [["6.3.5"], None],
                   "the release is asked for in the milestone 6.3.5, and when GitLab refuses it, created without one: %s"
                   % [p.get("milestones") for p in api.posted])

        # The milestone of the release's own name, and only that one, is linked, named
        # in the request: the job's token cannot read the Milestones API, which the
        # fake answers 401 as GitLab does, and nothing asks it.
        def milestones_linked(milestones, api_class=FakeGitLab):
            fake = api_class()
            fake.milestones = milestones
            try:
                linked = publish(signed, tag, publish_env, fake, section, verification, render_check=False).get("milestones")
            except ReleaseError as e:
                linked = "refused: %s" % e
            expect(not any(c[1].endswith("/milestones") for c in fake.calls), "the Milestones API, which a job's token cannot use, is not asked")
            return linked, len(fake.posted)

        expect(milestones_linked(["6.3.4", "6.3.5", "6.3.50"]) == (["6.3.5"], 1), "the release is created in the milestone 6.3.5")
        expect(milestones_linked(["6.3.50", "v6.3.5"]) == (None, 2), "a milestone whose title only resembles the version is not linked")

        class Refusing(FakeGitLab):
            """Answers a release POST that names a milestone with status and message."""
            answer = (422, "Validation failed: Milestone releases should belong to the same project as the release")

            def request(self, method, url, body=None, content_type=None):
                if method == "POST" and url.endswith("/releases") and "milestones" in json.loads(body.decode("utf-8")):
                    self.calls.append((method, url))
                    self.posted.append(json.loads(body.decode("utf-8")))
                    return self.answer[0], json.dumps({"message": self.answer[1]}).encode("utf-8"), {}
                return FakeGitLab.request(self, method, url, body, content_type)

        expect(milestones_linked(["6.3.5"], Refusing) == (None, 2), "a milestone GitLab refuses with a 422 still publishes, without one")

        class RefusingOther(Refusing):
            answer = (422, "Validation failed: Tag name is invalid")

        linked, posts = milestones_linked(["6.3.5"], RefusingOther)
        expect(str(linked).startswith("refused: creating the release v6.3.5 answered HTTP 422") and posts == 1,
               "a release GitLab refuses for another reason is not created again without the milestone: %s, %d POST(s)" % (linked, posts))

        # A retry after a run that stopped part-way: what is up already, with the
        # same bytes, is not sent again, and the release is made.
        retried = FakeGitLab()
        up_already = sorted(os.listdir(signed))[:5]
        for name in up_already:
            with open(os.path.join(signed, name), "rb") as f:
                retried.seed(name, f.read())
        try:
            publish(signed, tag, publish_env, retried, section, verification, render_check=False)
            sent = [urllib.parse.unquote(c[1].rsplit("/", 1)[1]) for c in retried.calls if c[0] == "PUT"]
            expect(len(sent) == 13 and not set(sent) & set(up_already), "a retry sends only what is not up already: %s" % sent)
            expect(all(len(v) == 1 for v in [[e for e in retried.files[(PACKAGE, version)] if e["file_name"] == n] for n in os.listdir(signed)]),
                   "a retry leaves one record of each file")
            expect(len(retried.releases) == 1, "a retry creates the release")
        except ReleaseError as e:
            failures.append("a retry after a publish that stopped part-way is published: %s" % e)

        # What publish refuses, having uploaded nothing.
        def refuses(what, needle, assets_dir=signed, env_changes=None, api_object=None):
            e = dict(publish_env)
            e.update(env_changes or {})
            fake = api_object or FakeGitLab()
            before = len(fake.calls)
            try:
                publish(assets_dir, tag, e, fake, section, verification, render_check=False)
                failures.append("%s is refused" % what)
            except ReleaseError as error:
                expect(needle in str(error), "%s is refused, saying why: %s" % (what, error))
            expect(not [c for c in fake.calls[before:] if c[0] in ("PUT", "POST")] or what.startswith("a registry"),
                   "%s uploads and creates nothing" % what)

        refuses("another project", "not %s" % PROJECT_ID, env_changes={"CI_PROJECT_ID": "1"})
        refuses("another GitLab", "which every release link names", env_changes={"CI_API_V4_URL": "https://gitlab.example.com/api/v4"})
        refuses("a release that exists already", "exists already", api_object=api)
        refuses("a file name the registry holds with other bytes", "already holds %s with the SHA-256" % names["rpm-x86_64"],
                api_object=FakeGitLab().seed(names["rpm-x86_64"], b"someone else's package"))
        with open(os.path.join(signed, names["windows"] + ".cosign.bundle"), "rb") as f:
            twice = FakeGitLab().seed(names["windows"] + ".cosign.bundle", f.read()).seed(names["windows"] + ".cosign.bundle", b"{}")
        refuses("a name held twice, once with these bytes and once with others", "already holds %s.cosign.bundle" % names["windows"],
                api_object=twice)
        mismatched = os.path.join(work, "mismatched")
        shutil.copytree(signed, mismatched)
        with open(os.path.join(mismatched, sbom_name(names["windows"])), "wb") as f:
            f.write(sbom_of(os.path.join(mismatched, names["deb-amd64"]), "windows-x64"))
        refuses("an installer SBOM that describes another file", "%s does not describe %s" % (sbom_name(names["windows"]), names["windows"]),
                assets_dir=mismatched)
        unsigned = os.path.join(work, "unsigned")
        shutil.copytree(signed, unsigned)
        os.remove(os.path.join(unsigned, names["windows"] + ".cosign.bundle"))
        refuses("a file without its bundle", "missing hMailServer-6.3.5-x64.exe.cosign.bundle", assets_dir=unsigned)
        extra = os.path.join(work, "extra")
        shutil.copytree(signed, extra)
        with open(os.path.join(extra, "notes.txt"), "w") as f:
            f.write("x")
        refuses("a file that is not a release's", "extra notes.txt", assets_dir=extra)

        class Tampering(FakeGitLab):
            def request(self, method, url, body=None, content_type=None):
                if method == "PUT" and "x64.exe?" in url:
                    body = body + b"!"
                return FakeGitLab.request(self, method, url, body, content_type)

        refuses("a registry that stores other bytes", "not %s" % sha256_of(os.path.join(signed, names["windows"])), api_object=Tampering())

        class Racing(FakeGitLab):
            """Someone uploads another file under a release's name while the job
            uploads: just before the job's own upload of it, or just after."""
            def __init__(self, before):
                FakeGitLab.__init__(self)
                self.before = before

            def request(self, method, url, body=None, content_type=None):
                racing = method == "PUT" and "amd64.deb?" in url
                if racing and self.before:
                    self.seed(names["deb-amd64"], b"slipped in")
                answer = FakeGitLab.request(self, method, url, body, content_type)
                if racing and not self.before:
                    self.seed(names["deb-amd64"], b"slipped in")
                return answer

        refuses("a registry that gains another file of a release's name after the job's upload of it",
                "the registry's %s has the SHA-256" % names["deb-amd64"], api_object=Racing(False))
        refuses("a registry that gains another file of a release's name before the job's upload of it, older than the job's",
                "the registry's %s has the SHA-256" % names["deb-amd64"], api_object=Racing(True))

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


# ---------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("command", nargs="?", choices=["collect", "sbom", "publish"])
    parser.add_argument("--out", default=None, help="collect: where the files go (release-assets); sbom: where the "
                        "installer's SBOM goes (release-sbom)")
    parser.add_argument("--dependencies", default=os.path.join("release-tools", DEPENDENCY_LIST),
                        help="sbom: build/native-dependencies.ps1's list, as linux-sbom wrote it")
    parser.add_argument("--installer-dir", default="release-authenticode", help="sbom: where the signed installer is")
    parser.add_argument("--assets", default="release-signed", help="publish: the signed files and their bundles")
    parser.add_argument("--verification", default=os.path.join("release-verified", "verification.md"),
                        help="publish: what release-verify wrote, for the notes")
    parser.add_argument("--self-test", action="store_true", help="test this script with no network, git or token")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    tag = os.environ.get("CI_COMMIT_TAG", "")
    try:
        if args.command == "sbom":
            version = version_of(tag)
            installer = feed.installer_names(version)["windows"]
            make_installer_sbom(ROOT, tag, os.environ, os.path.join(ROOT, "release-assets"),
                                os.path.join(ROOT, args.installer_dir, installer), os.path.join(ROOT, args.dependencies),
                                os.path.join(ROOT, args.out or "release-sbom"))
            return 0
        if args.command == "collect":
            token = os.environ.get("CI_JOB_TOKEN", "")
            problems = collect(ROOT, tag, os.path.join(ROOT, args.out or "release-assets"), os.environ, api=GitLab(token) if token else None)
            for problem in problems:
                print("REFUSED: " + problem)
            if problems:
                print("Nothing has been signed or published. %d problem(s)." % len(problems))
                return 1
            print("%s is ready to be signed: %d files in %s." % (tag, len(collected_assets(version_of(tag))), args.out or "release-assets"))
            return 0
        if args.command == "publish":
            with open(os.path.join(ROOT, CHANGELOG), encoding="utf-8") as f:
                notes = changelog_section(f.read(), version_of(tag))
            with open(os.path.join(ROOT, args.verification), encoding="utf-8") as f:
                verification = f.read()
            token = os.environ.get("CI_JOB_TOKEN", "")
            if not token:
                raise ReleaseError("there is no CI_JOB_TOKEN: publish runs in the release-publish job and nowhere else")
            publish(os.path.join(ROOT, args.assets), tag, os.environ, GitLab(token), notes, verification)
            return 0
    except (ReleaseError, feed.FeedError) as e:
        print("REFUSED: %s" % e)
        return 1
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
