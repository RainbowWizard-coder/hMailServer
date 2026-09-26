#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The two ends of a release's pipeline: what goes into it, and publishing it.

A v* tag's pipeline (.gitlab-ci.yml, the release stage; docs/ContinuousIntegration.md,
"The release") builds and tests, then waits for a maintainer to start the `release`
job. That job runs `collect`; the signing jobs follow it, and `release-publish`, also
started by hand, runs `publish`:

  collect   the tag is an annotated tag signed by a key in .github/allowed_signers,
            and it still points at the commit this pipeline built;
            Version.h says the version the tag names, so every file name made from
            it is the name a server looks for; the cosign image is gcr.io's pinned
            by digest, whatever a CI/CD variable made it, and the Azure settings the Authenticode job reads are all there; and the
            artefacts of the jobs it needs - the Windows installer, the .deb and the
            .rpm with their sums, the two SBOMs - are gathered into one directory
            under the names a release carries. Nothing leaves the pipeline.

  publish   every signed file and its <file>.cosign.bundle uploaded to the generic
            package hmailserver/<version> of this project, read back from the
            registry and held to the SHA-256 it was signed with, then the GitLab
            release created with one link per file, named after the file, which is
            what build/generate-update-feed.py and every server require. The notes
            are the version's section of CHANGELOG.md and the verification the
            release-verify job wrote. Last, the feed is rendered from the API as the
            pages job will render it, and the new release must be in it with its
            installer and bundle for Windows, Debian and RPM servers.

Neither asks for a secret: the pipeline's own CI_JOB_TOKEN uploads and creates the
release, and it is sent as a header and never printed. A release that exists already
is refused - a tag is spent once - and so is any project but this one.

Usage, from the repository root:
  python3 build/ci/release-chain.py collect [--out release-assets]
  python3 build/ci/release-chain.py publish [--assets release-signed] [--verification release-verified/verification.md]
  python3 build/ci/release-chain.py --self-test      no network, no git, no token
"""

import argparse
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

PROJECT_ID = feed.PROJECT_ID
PACKAGE = "hmailserver"
API_BASE = "https://gitlab.com/api/v4"
# The identity Fulcio writes for this project's pipeline of tag <tag>, which
# SigstoreVerifier.cpp's GITLAB_IDENTITY_PREFIX accepts. build/check-release-chain.py
# holds the two to each other.
SIGNER_IDENTITY_PREFIX = "https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/"
SIGNER_ISSUER = "https://gitlab.com"
VERSION_H = os.path.join("hmailserver", "source", "Server", "Common", "Application", "Version.h")
ALLOWED_SIGNERS = os.path.join(".github", "allowed_signers")
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


def expected_assets(version):
    """Every file a release carries, each signed beside it. The Linux AArch64
    packages and the AppImages are not built by the pipeline (docs/ContinuousIntegration.md)."""
    names = feed.installer_names(version)
    return [names["windows"], names["deb-amd64"], names["rpm-x86_64"], sums_name(version),
            "hmailserver.spdx.json", "hmailserver.cyclonedx.json"]


def sources(version):
    """Where collect finds each file: the artifacts of the jobs `release` needs."""
    names = feed.installer_names(version)
    return {
        names["windows"]: os.path.join("release-windows", names["windows"]),
        names["deb-amd64"]: os.path.join("linux-build", "packages", names["deb-amd64"]),
        names["rpm-x86_64"]: os.path.join("linux-build", "packages", names["rpm-x86_64"]),
        sums_name(version): os.path.join("linux-build", "packages", "SHA256SUMS.txt"),
        "hmailserver.spdx.json": os.path.join("sbom", "hmailserver.spdx.json"),
        "hmailserver.cyclonedx.json": os.path.join("sbom", "hmailserver.cyclonedx.json"),
    }


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

def check_tag(root, tag, commit, run=subprocess.run):
    """The tag is an annotated tag object signed by a key in .github/allowed_signers,
    and it points at the commit this pipeline built: a tag deleted and made again on
    another commit after the pipeline started is refused, not signed for."""
    problems = []

    def git(*arguments):
        return run(["git", "-C", root] + list(arguments), capture_output=True, text=True)

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
    verified = git("-c", "gpg.ssh.allowedSignersFile=" + ALLOWED_SIGNERS.replace(os.sep, "/"), "verify-tag", tag)
    if verified.returncode != 0:
        problems.append("%s is not signed by a key in %s: %s" % (tag, ALLOWED_SIGNERS, (verified.stderr or verified.stdout).strip()[:400]))
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


def collect(root, tag, out, env, run=subprocess.run):
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

    places = sources(version)
    for name in expected_assets(version):
        path = os.path.join(root, places[name])
        if not os.path.isfile(path) or os.path.getsize(path) == 0:
            problems.append("%s is not in this job's artifacts (%s): the job that makes it failed or did not run" % (name, places[name]))
    if not problems:
        names = feed.installer_names(version)
        problems.extend(check_sums(os.path.join(root, places[sums_name(version)]),
                                   os.path.join(root, places[names["deb-amd64"]]),
                                   os.path.join(root, places[names["rpm-x86_64"]])))
    if problems:
        return problems

    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    for name in expected_assets(version):
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


def registry_record(api, version):
    """The registry's newest record of every file of hmailserver/<version>."""
    packages = feed.get_all(api.feed_get, "/packages?package_type=generic&package_name=%s" % PACKAGE)
    record = {}
    for package in packages:
        if package.get("name") != PACKAGE or package.get("version") != version:
            continue
        for entry in feed.get_all(api.feed_get, "/packages/%d/package_files" % package["id"]):
            name = entry.get("file_name")
            if name not in record or entry.get("id", 0) > record[name].get("id", 0):
                record[name] = entry
    return record


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

    status, body, _ = api.request("GET", "%s/releases/%s" % (feed.API, urllib.parse.quote(tag, safe="")))
    if status == 200:
        raise ReleaseError("the release %s exists already. A tag is spent once: nothing was uploaded, and nothing on "
                           "it was changed" % tag)
    if status != 404:
        raise ReleaseError("asking whether %s exists answered HTTP %d: %s" % (tag, status, message_of(body)))

    digests = {}
    for name in sorted(wanted):
        path = os.path.join(assets, name)
        digests[name] = sha256_of(path)
        with open(path, "rb") as f:
            data = f.read()
        status, body, _ = api.request("PUT", registry_url(version, name) + "?select=package_file", data, "application/octet-stream")
        if status not in (200, 201):
            raise ReleaseError("uploading %s answered HTTP %d: %s. Nothing is released until every file is up; run the "
                               "job again" % (name, status, message_of(body)))
        say("  uploaded %s (%d bytes)" % (name, len(data)))

    record = registry_record(api, version)
    for name in sorted(wanted):
        entry = record.get(name)
        if entry is None:
            raise ReleaseError("the registry has no %s in %s/%s after its upload" % (name, PACKAGE, version))
        if (entry.get("file_sha256") or "").lower() != digests[name]:
            raise ReleaseError("the registry's %s has the SHA-256 %s, not %s, the file that was signed" % (
                name, entry.get("file_sha256"), digests[name]))
    say("the registry holds all %d files with the digests that were signed" % len(wanted))

    description = "# hMailServer %s\n\n%s\n\n%s\n" % (version, notes.strip(), verification.strip())
    payload = {"tag_name": tag, "name": "hMailServer %s" % version, "description": description,
               "assets": {"links": release_links(version, wanted)}}
    status, body, _ = api.request("POST", feed.API + "/releases", json.dumps(payload).encode("utf-8"), "application/json")
    if status not in (200, 201):
        raise ReleaseError("creating the release %s answered HTTP %d: %s. The files are in the registry; run the job "
                           "again once the cause is fixed" % (tag, status, message_of(body)))
    say("created the release %s with %d links" % (tag, len(wanted)))

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
        deb = os.path.join(root, sources(version)[names["deb-amd64"]])
        rpm = os.path.join(root, sources(version)[names["rpm-x86_64"]])
        sums = "%s  %s\n%s  %s\n" % (sha256_of(deb), names["deb-amd64"], sha256_of(rpm), names["rpm-x86_64"])
        write(sources(version)[sums_name(version)], sums.encode("utf-8"))

        built = "c" * 40
        env = {"HM_COSIGN_IMAGE": "gcr.io/projectsigstore/cosign:v2.6.1-dev@sha256:" + "a" * 64, "CI_COMMIT_SHA": built}
        for name in AZURE_SETTINGS:
            env[name] = "set"

        class Done(object):
            def __init__(self, code, out="", err=""):
                self.returncode, self.stdout, self.stderr = code, out, err

        def tag_at(commit):
            def answer(arguments, **kwargs):
                if "cat-file" in arguments:
                    return Done(0, "tag\n")
                if "rev-parse" in arguments:
                    return Done(0, commit + "\n")
                return Done(0)
            return answer

        signed_tag = tag_at(built)

        out = os.path.join(work, "release-assets")
        problems = collect(root, tag, out, env, run=signed_tag)
        expect(problems == [], "a complete, signed, stamped release is collected: %s" % problems)
        expect(sorted(os.listdir(out)) == sorted(expected_assets(version)), "under the names a release carries: %s" % (
            sorted(os.listdir(out)) if os.path.isdir(out) else "nothing"))

        def refused(what, needle, **changes):
            e = dict(env)
            e.update(changes.get("env", {}))
            run = changes.get("run", signed_tag)
            found = collect(root, changes.get("tag", tag), os.path.join(work, "refused"), e, run=run)
            expect(any(needle in p for p in found), "%s is refused: %s" % (what, found))
            expect(not os.path.isdir(os.path.join(work, "refused")), "%s copies nothing" % what)

        refused("a tag Version.h does not name", "Version.h says 6.3.5", tag="v6.3.6")
        refused("a cosign image left unpinned", "HM_COSIGN_IMAGE", env={"HM_COSIGN_IMAGE": "gcr.io/projectsigstore/cosign:v2.6.1-dev@sha256:PIN-THE-DIGEST"})
        refused("a cosign image from ghcr.io, pinned, as a CI/CD variable could set it", "not gcr.io/projectsigstore/cosign",
                env={"HM_COSIGN_IMAGE": "ghcr.io/sigstore/cosign/cosign:v2.6.1-dev@sha256:" + "a" * 64})
        refused("a tag moved to another commit after the pipeline started", "the tag was moved", run=tag_at("d" * 40))
        refused("a collect outside a pipeline", "CI_COMMIT_SHA is", env={"CI_COMMIT_SHA": ""})
        refused("an Azure setting left out", "ARTIFACT_SIGNING_PROFILE", env={"ARTIFACT_SIGNING_PROFILE": " "})
        refused("a lightweight tag", "is not an annotated tag", run=lambda a, **k: Done(0, "commit\n"))
        refused("a tag signed by nobody allowed", "is not signed by a key",
                run=lambda a, **k: Done(0, "tag\n") if "cat-file" in a else Done(1 if "verify-tag" in a else 0, "", "no principal matched"))
        write(sources(version)[sums_name(version)], ("%s  %s\n" % ("0" * 64, names["deb-amd64"])).encode("utf-8"))
        refused("a sums file that is not the packages'", "is not the sums of the two packages")
        write(sources(version)[sums_name(version)], sums.encode("utf-8"))
        os.remove(os.path.join(root, sources(version)["hmailserver.spdx.json"]))
        refused("a missing SBOM", "hmailserver.spdx.json is not in this job's artifacts")

        # Publishing, against a registry and a releases API held in memory.
        signed = os.path.join(work, "release-signed")
        shutil.copytree(out, signed)
        for name in expected_assets(version):
            with open(os.path.join(signed, name + ".cosign.bundle"), "w") as f:
                f.write('{"mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json"}')

        class FakeGitLab(object):
            def __init__(self):
                self.files = {}
                self.releases = []
                self.calls = []
                self.next_id = [100]

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
            expect(len(links) == 12 and all(l["name"] == urllib.parse.unquote(feed.REGISTRY_LINK.match(l["url"]).group(3)) for l in links),
                   "twelve links, each named after the file it points at")
            expect(len(set(l["direct_asset_path"] for l in links)) == 12, "every permanent download path is its own")
            expect(payload["name"] == "hMailServer 6.3.5" and "The notes." in payload["description"] and SIGNER_IDENTITY_PREFIX in payload["description"],
                   "the notes are the CHANGELOG section and the verification")
            uploads = [c for c in api.calls if c[0] == "PUT"]
            created = [i for i, c in enumerate(api.calls) if c[0] == "POST"]
            expect(len(uploads) == 12 and created and all(api.calls.index(u) < created[0] for u in uploads),
                   "every file is uploaded before the release links to it")
            # And the feed, rendered from what was published, gives each kind of server its file.
            tree, latest, warnings = feed.render(*feed.load(api.feed_get))
            expect("releases/tags/v6.3.5" in tree and latest["tag_name"] == "v6.3.5" and not warnings,
                   "the feed publishes the release as the stable one: %s" % warnings)

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

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


# ---------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("command", nargs="?", choices=["collect", "publish"])
    parser.add_argument("--out", default="release-assets", help="collect: where the files go")
    parser.add_argument("--assets", default="release-signed", help="publish: the signed files and their bundles")
    parser.add_argument("--verification", default=os.path.join("release-verified", "verification.md"),
                        help="publish: what release-verify wrote, for the notes")
    parser.add_argument("--self-test", action="store_true", help="test this script with no network, git or token")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    tag = os.environ.get("CI_COMMIT_TAG", "")
    try:
        if args.command == "collect":
            problems = collect(ROOT, tag, os.path.join(ROOT, args.out), os.environ)
            for problem in problems:
                print("REFUSED: " + problem)
            if problems:
                print("Nothing has been signed or published. %d problem(s)." % len(problems))
                return 1
            print("%s is ready to be signed: %d files in %s." % (tag, len(expected_assets(version_of(tag))), args.out))
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
