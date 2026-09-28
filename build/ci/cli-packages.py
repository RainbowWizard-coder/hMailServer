#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The command-line clients as packages: hmctl on PyPI, the PowerShell module on the
PowerShell Gallery (hmailserver/docs/CommandLinePackages.md; .gitlab/ci/cli-packages.yml).

  build              hmailserver-cli's sdist and wheel, from a tree: its
                     hmailserver/source/Tools/Cli/python/ (pyproject.toml and the
                     README) copied to a staging directory, with the three files that
                     make the package there - hmailserver_cli/hmctl.py, which is
                     Tools/Cli/hmctl byte for byte, the file the .deb and the .rpm
                     install as /usr/bin/hmctl; hmailserver_cli/__init__.py, whose
                     __version__ is Common/Application/Version.h's; and the
                     repository's LICENSE - and hmailserver_cli/__main__.py. Then
                     `python -m build` makes both, and build/check-cli-packages.py
                     holds them to the tree: the version, hmctl's bytes, no
                     dependency, no file that runs at install. The text files are
                     staged with LF endings whatever the checkout has, and
                     SOURCE_DATE_EPOCH is the tree's last commit's time unless it is
                     set, so the same tree makes the same bytes.

  pypi-publish       the release's hmailserver-cli, to PyPI through a trusted
                     publisher, with a PyPI publish attestation (Sigstore) for each
                     file. The release is HM_CHANNEL_VERSION when that project
                     variable is set, and otherwise the update feed's
                     releases/latest, and the feed must list it as a published
                     release, not a draft or a pre-release; its tag must be an
                     annotated tag on master signed by the key master's
                     .gitlab/allowed_signers lists (the release job's own check,
                     build/ci/release-chain.py), and the package is built from that
                     tag's tree, not from the commit the job runs on. Dormant: when
                     PyPI has no trusted publisher for this pipeline and no project
                     of that name it says "not configured" and exits 0; a publisher
                     that has stopped matching a project PyPI has fails the job.
                     When PyPI has both files of the version it says so and exits 0;
                     when it has one, an upload stopped part-way, it sends the other.

  psgallery-publish  the release's HmailServer module to the PowerShell Gallery with
                     the key in PSGALLERY_API_KEY: the same release, the same tag
                     check, the module's two files from the tag's tree, the manifest
                     held to the version first, and an unsigned module refused (the
                     Gallery is to have the one the release signs, Roadmap3 row 99).
                     Dormant: without the key it says "not configured" and exits 0,
                     and a version the Gallery has already is not sent again.

Neither publish command prints a credential or passes one on a command line: the
PyPI token minted from the job's ID token reaches twine in its environment, and the
Gallery key reaches Publish-PSResource from pwsh's own environment.

Usage, from the root of a checkout:
  python3 build/ci/cli-packages.py build [--tree DIR] [--out DIR] [--isolation]
  python3 build/ci/cli-packages.py pypi-publish [--out DIR]
  python3 build/ci/cli-packages.py psgallery-publish [--out DIR]
  python3 build/ci/cli-packages.py --self-test
"""

import argparse
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

DIST_NAME = "hmailserver-cli"
MODULE = "hmailserver_cli"
GALLERY_NAME = "HmailServer"
FEED = "https://updates.progressiverobot.com/releases/"
FEED_LATEST = FEED + "latest"
FEED_TAG = FEED + "tags/%s"
PYPI = "https://pypi.org"
GALLERY = "https://www.powershellgallery.com"
USER_AGENT = "hmailserver-cli-packages (+https://gitlab.com/hmailserver/hmailserver)"
# How Set-AuthenticodeSignature opens the block it appends to a PowerShell file.
SIGNATURE_BLOCK = b"# SIG # Begin signature block"

# What a release's tree must hold for each channel.
TREE_PATHS = [
    "LICENSE",
    "hmailserver/source/Server/Common/Application/Version.h",
    "hmailserver/source/Tools/Cli",
]

INIT_TEMPLATE = '''# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""hmailserver-cli: hmctl, the command-line client of hMailServer's REST API.

The command is hmailserver_cli.hmctl:main. hmailserver_cli/hmctl.py is the file the
server's Linux packages install as /usr/bin/hmctl (hmailserver/source/Tools/Cli/hmctl
in https://gitlab.com/hmailserver/hmailserver), copied byte for byte when the package
was built, and the version is the server's.
"""

__version__ = "{version}"
'''

MAIN_TEMPLATE = '''# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""python -m hmailserver_cli: the same as the hmctl command."""

import sys

from hmailserver_cli.hmctl import main

if __name__ == "__main__":
    sys.exit(main())
'''


class Refused(Exception):
    """A reason to stop that is the operator's to act on, not a crash."""


def load(name, relative, root=ROOT):
    """A module of this repository by its path (the checkers' names have hyphens)."""
    path = os.path.join(root, relative)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def checker():
    return load("check_cli_packages", os.path.join("build", "check-cli-packages.py"))


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def lf(data):
    return data.replace(b"\r\n", b"\n")


# ---------------------------------------------------------------- build

def stage(tree, where):
    """The package's source directory, made in `where` from `tree`: what the sdist
    will hold. Answers the version."""
    check = checker()
    version = check.read_version(tree)
    source = os.path.join(tree, check.PYTHON_DIR)
    if not os.path.isdir(source):
        raise Refused("%s has no %s: the first tree with the package is 6.3.6's" % (tree, check.PYTHON_DIR))
    problems = check.check_python_dir(tree) + check.check_pyproject(tree) + check.check_hmctl(tree)
    if problems:
        raise Refused("the tree's package sources are not what build/check-cli-packages.py holds them to:\n  "
                      + "\n  ".join(problems))
    if os.path.exists(where):
        shutil.rmtree(where)
    os.makedirs(os.path.join(where, MODULE))
    for name in sorted(check.COMMITTED_PYTHON_FILES):
        with open(os.path.join(source, name), "rb") as f:
            data = lf(f.read())
        with open(os.path.join(where, name), "wb") as f:
            f.write(data)
    with open(os.path.join(tree, "LICENSE"), "rb") as f:
        licence = lf(f.read())
    with open(os.path.join(where, "LICENSE"), "wb") as f:
        f.write(licence)
    # hmctl is checked out LF on every platform (.gitattributes), and copied as it is:
    # the same bytes as /usr/bin/hmctl, which build/check-cli-packages.py compares.
    shutil.copyfile(os.path.join(tree, check.HMCTL), os.path.join(where, MODULE, "hmctl.py"))
    with open(os.path.join(where, MODULE, "__init__.py"), "w", encoding="utf-8", newline="\n") as f:
        f.write(INIT_TEMPLATE.format(version=version))
    with open(os.path.join(where, MODULE, "__main__.py"), "w", encoding="utf-8", newline="\n") as f:
        f.write(MAIN_TEMPLATE)
    return version


def source_date_epoch(tree):
    if os.environ.get("SOURCE_DATE_EPOCH"):
        return os.environ["SOURCE_DATE_EPOCH"]
    try:
        answer = subprocess.run(["git", "-C", tree, "log", "-1", "--format=%ct"], capture_output=True, text=True)
    except OSError:
        return None
    value = answer.stdout.strip()
    return value if answer.returncode == 0 and value.isdigit() else None


def build(tree, out, isolation=False, epoch=None):
    """Stages the package, builds its sdist and wheel into out/dist, and holds them
    to the tree. Answers (version, [paths])."""
    tree = os.path.abspath(tree)
    out = os.path.abspath(out)
    staged = os.path.join(out, "stage")
    dist = os.path.join(out, "dist")
    version = stage(tree, staged)
    if os.path.exists(dist):
        shutil.rmtree(dist)
    os.makedirs(dist)
    environment = dict(os.environ)
    epoch = epoch or source_date_epoch(tree)
    if epoch:
        environment["SOURCE_DATE_EPOCH"] = epoch
    else:
        print("note: no SOURCE_DATE_EPOCH and no git history to take one from, so the files carry the time they"
              " are built at, and a second build is other bytes")
    # Neither --sdist nor --wheel: build's default makes the sdist and then the wheel
    # FROM the sdist, which proves the sdist holds everything the package needs.
    command = [sys.executable, "-m", "build", "--outdir", dist]
    if not isolation:
        command.append("--no-isolation")
    command.append(staged)
    print("$ " + " ".join(command) + ("   (SOURCE_DATE_EPOCH=%s)" % epoch if epoch else ""))
    sys.stdout.flush()
    if subprocess.run(command, env=environment).returncode != 0:
        raise Refused("python -m build failed")
    check = checker()
    with open(os.path.join(tree, check.HMCTL), "rb") as f:
        hmctl = f.read()
    problems = check.check_dist(dist, version, hmctl)
    if problems:
        raise Refused("the built files are not what build/check-cli-packages.py holds them to:\n  " + "\n  ".join(problems))
    files = sorted(os.path.join(dist, n) for n in os.listdir(dist))
    for path in files:
        print("%s  %s" % (sha256(path), os.path.basename(path)))
    return version, files


# ---------------------------------------------------------------- the release

def http_json(url, data=None, headers=None):
    """(status, decoded JSON or None) for a GET, or a POST when data is given."""
    request = urllib.request.Request(url, data=data, headers=dict({"User-Agent": USER_AGENT,
                                                                   "Accept": "application/json"}, **(headers or {})))
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            body = response.read()
            status = response.status
    except urllib.error.HTTPError as error:
        body = error.read()
        status = error.code
    try:
        return status, json.loads(body.decode("utf-8")) if body else None
    except ValueError:
        return status, None


def release_version(environ=None, fetch=http_json):
    """The release to publish, as (version, where): the project variable
    HM_CHANNEL_VERSION when it is set, otherwise the feed's newest release that is
    not a pre-release. Either way the update feed must list it as a published
    release and not a draft or a pre-release: a signed tag whose release pipeline
    failed or has not finished is no release, and a version a channel has had it
    keeps for good."""
    environ = os.environ if environ is None else environ
    wanted = (environ.get("HM_CHANNEL_VERSION") or "").strip()
    if wanted:
        where = "HM_CHANNEL_VERSION"
        version = wanted[1:] if wanted.startswith("v") else wanted
        if not re.match(r"^\d+\.\d+\.\d+$", version):
            raise Refused("%s names %r: the channels take a release version, X.Y.Z, and never a pre-release"
                          % (where, wanted))
        url = FEED_TAG % ("v" + version)
    else:
        where = url = FEED_LATEST
        version = None
    status, answer = fetch(url)
    if status != 200 or not isinstance(answer, dict) or not answer.get("tag_name"):
        raise Refused("the update feed's %s answered %s: %s" % (
            url, status, "the feed lists no such release" if version else "no release to publish"))
    tag = str(answer["tag_name"])
    found = tag[1:] if tag.startswith("v") else tag
    if not re.match(r"^\d+\.\d+\.\d+$", found):
        raise Refused("%s names %r: the channels take a release version, X.Y.Z, and never a pre-release" % (url, tag))
    if version is not None and found != version:
        raise Refused("the update feed's %s is the release %s, not v%s" % (url, tag, version))
    if answer.get("draft") or answer.get("prerelease"):
        raise Refused("the update feed lists %s as a %s: the channels take a published release"
                      % (tag, "draft" if answer.get("draft") else "pre-release"))
    return found, where


def release_tree(version, into, root=ROOT, run=subprocess.run):
    """The tree of the tag v<version>, as far as the packages need it, in `into`,
    after the release job's own check of the tag."""
    tag = "v" + version

    def git(*arguments, **kwargs):
        return run(["git", "-C", root] + list(arguments), capture_output=True, **kwargs)

    fetched = git("fetch", "--force", "--no-tags", "origin", "+refs/tags/%s:refs/tags/%s" % (tag, tag), text=True)
    if fetched.returncode != 0:
        raise Refused("the tag %s could not be fetched: %s" % (tag, (fetched.stderr or fetched.stdout).strip()[:300]))
    target = git("rev-parse", "--verify", "--quiet", "refs/tags/%s^{commit}" % tag, text=True).stdout.strip()
    chain = load("release_chain", os.path.join("build", "ci", "release-chain.py"), root)
    problems = chain.check_tag(root, tag, target, run=run)
    if problems:
        raise Refused("%s is not a release tag this job publishes:\n  %s" % (tag, "\n  ".join(problems)))
    print("%s: an annotated tag on master, at %s, signed by the key master's .gitlab/allowed_signers lists" % (tag, target))
    # The commit the check was made of, by its hash, not the tag's name again.
    archive = git("archive", "--format=tar", target, "--", *TREE_PATHS)
    if archive.returncode != 0:
        raise Refused("git archive of %s failed: %s" % (tag, archive.stderr.decode("utf-8", "replace").strip()[:300]))
    if os.path.exists(into):
        shutil.rmtree(into)
    os.makedirs(into)
    with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as tar:
        if hasattr(tarfile, "data_filter"):
            tar.extractall(into, filter="data")
        else:
            tar.extractall(into)
    return target


# ---------------------------------------------------------------- PyPI

def pypi_files(version, fetch=http_json):
    """{filename: sha256} of what PyPI holds of hmailserver-cli <version>; {} if none."""
    status, answer = fetch("%s/pypi/%s/%s/json" % (PYPI, DIST_NAME, version))
    if status == 404:
        return {}
    if status != 200 or not isinstance(answer, dict):
        raise Refused("PyPI's JSON API answered %s for %s %s" % (status, DIST_NAME, version))
    return dict((u["filename"], u.get("digests", {}).get("sha256", "")) for u in answer.get("urls", []))


def mint_pypi_token(id_token, fetch=http_json):
    """PyPI's short-lived upload token for this pipeline's ID token, or None when PyPI
    has no trusted publisher that matches it."""
    status, answer = fetch(PYPI + "/_/oidc/mint-token", data=json.dumps({"token": id_token}).encode("utf-8"),
                           headers={"Content-Type": "application/json"})
    if status == 200 and isinstance(answer, dict) and answer.get("token"):
        return answer["token"]
    errors = answer.get("errors", []) if isinstance(answer, dict) else []
    codes = [e.get("code") for e in errors if isinstance(e, dict)]
    if "invalid-publisher" in codes:
        return None
    described = "; ".join("%s: %s" % (e.get("code"), e.get("description")) for e in errors if isinstance(e, dict))
    raise Refused("PyPI refused the pipeline's ID token (%s): %s" % (status, described or "no reason given"))


def pypi_publish(out, environ=None, fetch=http_json, run=subprocess.run, root=ROOT, tree_fn=release_tree):
    environ = os.environ if environ is None else environ

    # Set up at all? Asked first, so that until PyPI has the trusted publisher the job
    # says so and passes, whatever else is true.
    id_token = environ.get("PYPI_ID_TOKEN")
    if not id_token:
        raise Refused("the job has no PYPI_ID_TOKEN: its id_tokens entry (aud: pypi) is missing")
    token = mint_pypi_token(id_token, fetch)
    if token is None:
        # PyPI says invalid-publisher both when no publisher was ever added and when the
        # one added no longer matches (the project moved, the file or the environment
        # was renamed). Once PyPI has the project, it is the second, and a job that
        # passed saying "not configured" would hide a channel that has stopped.
        status, _ = fetch("%s/pypi/%s/json" % (PYPI, DIST_NAME))
        if status == 200:
            raise Refused("PyPI has %s, and none of its trusted publishers matches this pipeline's ID token (project %s,"
                          " file .gitlab-ci.yml, environment pypi): the publisher on PyPI or the project here has"
                          " changed. Nothing was published." % (DIST_NAME, environ.get("CI_PROJECT_PATH") or "?"))
        if status != 404:
            raise Refused("PyPI has no trusted publisher for this pipeline, and its JSON API answered %s for %s, so"
                          " whether that is a publisher never added or one that has stopped matching is unknown."
                          " Nothing was published." % (status, DIST_NAME))
        print("not configured: PyPI has no trusted publisher for this pipeline, so nothing was published.")
        print("A PyPI owner of %s adds one (hmailserver/docs/CommandLinePackages.md, 'What the owner sets'):" % DIST_NAME)
        namespace, _, project = (environ.get("CI_PROJECT_PATH") or "hmailserver/hmailserver").rpartition("/")
        print("  namespace %s, project %s, top-level pipeline file .gitlab-ci.yml, environment pypi"
              % (namespace, project))
        return 0

    version, where = release_version(environ, fetch)
    print("the release: %s (from %s)" % (version, where))
    names = ["%s-%s.tar.gz" % (MODULE, version), "%s-%s-py3-none-any.whl" % (MODULE, version)]
    held = pypi_files(version, fetch)
    if held:
        print("PyPI has these files of %s %s already, and a file PyPI has had is never replaced:" % (DIST_NAME, version))
        for name, digest in sorted(held.items()):
            print("  %s  %s" % (digest, name))
    if all(name in held for name in names):
        print("Nothing to publish.")
        return 0
    if held:
        # twine sends one file at a time, so an upload that failed part-way leaves
        # the version with one of its two files; this run sends the other.
        print("and not %s: an upload that stopped part-way, which this run finishes."
              % ", ".join(n for n in names if n not in held))

    tree = os.path.join(out, "tree")
    target = tree_fn(version, tree, root=root, run=run)
    # The tag commit's time, so that the files are the bytes any later build of the
    # tag makes. The extracted tree is inside this checkout, where git would answer
    # with the time of the commit the pipeline is for.
    stamp = run(["git", "-C", root, "log", "-1", "--format=%ct", target], capture_output=True, text=True).stdout.strip()
    if not stamp.isdigit():
        raise Refused("git could not say when %s was committed" % target)
    version_built, files = build(tree, out, isolation=False, epoch=stamp)
    if version_built != version:
        raise Refused("v%s's Version.h says %s" % (version, version_built))

    for path in files:
        name = os.path.basename(path)
        if name in held and held[name] != sha256(path):
            print("note: PyPI's %s is other bytes than this build of v%s makes (PyPI %s, here %s); PyPI's stays"
                  % (name, version, held[name], sha256(path)))
    files = [path for path in files if os.path.basename(path) not in held]

    # The upload token PyPI minted lives fifteen minutes; the tag check and the build
    # take seconds.
    if not environ.get("SIGSTORE_ID_TOKEN"):
        raise Refused("the job has no SIGSTORE_ID_TOKEN: its id_tokens entry (aud: sigstore) is missing")
    signed = run([sys.executable, "-m", "pypi_attestations", "sign"] + files)
    if signed.returncode != 0:
        raise Refused("pypi-attestations could not sign the files")
    for path in files:
        if not os.path.isfile(path + ".publish.attestation"):
            raise Refused("no attestation was written for %s" % os.path.basename(path))

    upload_environment = dict(environ)
    upload_environment.update({"TWINE_USERNAME": "__token__", "TWINE_PASSWORD": token,
                               "TWINE_NON_INTERACTIVE": "1",
                               "PYTHON_KEYRING_BACKEND": "keyring.backends.null.Keyring"})
    # twine pairs each file with the <file>.publish.attestation named beside it on its
    # command line; --attestations with a file that has none is refused.
    # --skip-existing: PyPI's JSON API is served through a cache and can say a file is
    # not there that is; twine then skips it, and the read-back below still holds the
    # file PyPI has to the bytes built here.
    attestations = [path + ".publish.attestation" for path in files]
    uploaded = run([sys.executable, "-m", "twine", "upload", "--attestations", "--skip-existing", "--non-interactive",
                    "--disable-progress-bar"] + files + attestations, env=upload_environment)
    token = None
    upload_environment = None
    if uploaded.returncode != 0:
        raise Refused("twine upload failed")

    # PyPI's JSON API is served through a cache: asked again for a minute before a
    # file it does not show yet is taken for one it does not have.
    for attempt in range(7):
        held = pypi_files(version, fetch)
        missing = [os.path.basename(p) for p in files if held.get(os.path.basename(p)) != sha256(p)]
        if not missing:
            break
        if attempt < 6:
            time.sleep(10)
    if missing:
        raise Refused("PyPI does not show %s with the bytes uploaded" % ", ".join(missing))
    for path in files:
        name = os.path.basename(path)
        status, _ = fetch("%s/integrity/%s/%s/%s/provenance" % (PYPI, DIST_NAME, version, name))
        print("%s: on PyPI, provenance %s" % (name, "recorded" if status == 200 else "not shown yet (HTTP %s)" % status))
    print("Published %s %s: %s/project/%s/%s/" % (DIST_NAME, version, PYPI, DIST_NAME, version))
    return 0


# ---------------------------------------------------------------- the PowerShell Gallery

def gallery_versions(fetch_text=None):
    """The versions of HmailServer the Gallery lists (its v2 OData feed)."""
    url = "%s/api/v2/FindPackagesById()?id='%s'" % (GALLERY, GALLERY_NAME)
    if fetch_text is None:
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                text = response.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return set()
            raise Refused("the Gallery answered %s for %s" % (error.code, url))
    else:
        text = fetch_text(url)
    return set(re.findall(r"<d:Version>([^<]+)</d:Version>", text))


def psgallery_publish(out, environ=None, fetch=http_json, run=subprocess.run, root=ROOT, fetch_text=None,
                      tree_fn=release_tree):
    environ = os.environ if environ is None else environ

    # Set up at all? Asked first, as pypi-publish does.
    if not environ.get("PSGALLERY_API_KEY"):
        print("not configured: PSGALLERY_API_KEY is not set, so nothing was published.")
        print("A maintainer makes a Gallery API key for %s and stores it as a protected, masked CI/CD variable"
              % GALLERY_NAME)
        print("(hmailserver/docs/CommandLinePackages.md, 'What the owner sets').")
        return 0

    version, where = release_version(environ, fetch)
    print("the release: %s (from %s)" % (version, where))
    if version in gallery_versions(fetch_text):
        print("The Gallery has %s %s already; a version is published once. Nothing to publish." % (GALLERY_NAME, version))
        return 0

    tree = os.path.join(out, "tree")
    tree_fn(version, tree, root=root, run=run)
    check = checker()
    source = os.path.join(tree, check.CLI)
    if not os.path.isfile(os.path.join(source, "HmailServer.psd1")):
        raise Refused("v%s has no HmailServer.psd1: the first release with the manifest is 6.3.6" % version)
    problems = check.check_manifest(tree, version)
    if problems:
        raise Refused("v%s's module manifest is not what build/check-cli-packages.py holds it to:\n  %s"
                      % (version, "\n  ".join(problems)))
    # The Gallery is to have the module Authenticode-signed (Roadmap3, "PowerShell
    # Gallery": the manifest and the signature are the tag's), and a version it has
    # it keeps for good, so an unsigned module is never sent. The signature is made in
    # the release, not in the tree; until this job takes the module the release signed,
    # it refuses here, before anything is sent.
    with open(os.path.join(source, "HmailServer.psm1"), "rb") as f:
        if SIGNATURE_BLOCK not in f.read():
            raise Refused("v%s's HmailServer.psm1 carries no Authenticode signature block: the Gallery takes the module"
                          " the release signs, and a version it has had is never replaced, so nothing unsigned is sent."
                          " Nothing was published." % version)
    module = os.path.join(out, "module", GALLERY_NAME)
    if os.path.exists(os.path.dirname(module)):
        shutil.rmtree(os.path.dirname(module))
    os.makedirs(module)
    for name in ("HmailServer.psd1", "HmailServer.psm1"):
        shutil.copyfile(os.path.join(source, name), os.path.join(module, name))
        print("%s  %s" % (sha256(os.path.join(module, name)), name))

    # The key is read from pwsh's own environment; this command line names the
    # variable and never its value.
    script = ("$ErrorActionPreference = 'Stop'; "
              "Publish-PSResource -Path '%s' -Repository PSGallery -ApiKey $env:PSGALLERY_API_KEY"
              % module.replace("'", "''"))
    published = run(["pwsh", "-NoProfile", "-NonInteractive", "-Command", script], env=dict(environ))
    if published.returncode != 0:
        raise Refused("Publish-PSResource failed")
    print("Published %s %s: %s/packages/%s/%s (the Gallery scans a new version before it lists it)"
          % (GALLERY_NAME, version, GALLERY, GALLERY_NAME, version))
    return 0


# ---------------------------------------------------------------- self-test

def self_test():
    failures = []

    def expect(what, condition):
        print("%s  %s" % ("ok  " if condition else "FAIL", what))
        if not condition:
            failures.append(what)

    def refused(what, call, needle):
        try:
            call()
        except Refused as error:
            expect("%s (%s)" % (what, str(error).splitlines()[0][:100]), needle in str(error))
            return
        expect(what + ": not refused", False)

    # The release, from the variable and from the feed, and the feed's word that it is one.
    def feed(releases):
        asked = []

        def fetch(url):
            asked.append(url)
            for tag, answer in releases.items():
                if url in (FEED_TAG % tag,) or (url == FEED_LATEST and tag == "latest"):
                    return 200, answer
            return 404, None
        return fetch, asked

    fetch, asked = feed({"v6.3.6": {"tag_name": "v6.3.6", "draft": False, "prerelease": False}})
    expect("HM_CHANNEL_VERSION wins, and the feed lists it",
           release_version({"HM_CHANNEL_VERSION": "v6.3.6"}, fetch)[0] == "6.3.6" and asked == [FEED_TAG % "v6.3.6"])
    expect("the feed's latest otherwise",
           release_version({}, feed({"latest": {"tag_name": "v6.3.5", "prerelease": False}})[0])[0] == "6.3.5")
    refused("a pre-release", lambda: release_version({"HM_CHANNEL_VERSION": "6.3.6-rc1"}, None), "never a pre-release")
    refused("the feed down", lambda: release_version({}, lambda url: (503, None)), "no release to publish")
    refused("HM_CHANNEL_VERSION naming a tag the feed has no release of (a release that failed or is not out yet)",
            lambda: release_version({"HM_CHANNEL_VERSION": "6.3.7"}, feed({})[0]), "the feed lists no such release")
    refused("a release the feed marks a pre-release",
            lambda: release_version({"HM_CHANNEL_VERSION": "6.3.6"}, feed({"v6.3.6": {
                "tag_name": "v6.3.6", "prerelease": True}})[0]), "as a pre-release")
    refused("a draft", lambda: release_version({}, feed({"latest": {"tag_name": "v6.3.6", "draft": True}})[0]),
            "as a draft")
    refused("the feed answering another release for the tag",
            lambda: release_version({"HM_CHANNEL_VERSION": "6.3.6"}, feed({"v6.3.6": {"tag_name": "v6.3.5"}})[0]),
            "not v6.3.6")

    # The trusted publisher's answers.
    expect("a token when PyPI has a publisher",
           mint_pypi_token("x", lambda *a, **k: (200, {"success": True, "token": "pypi-abc"})) == "pypi-abc")
    expect("None when it has none: dormant",
           mint_pypi_token("x", lambda *a, **k: (422, {"message": "Token request failed", "errors": [
               {"code": "invalid-publisher", "description": "valid token, but no corresponding publisher"}]})) is None)
    refused("any other refusal fails the job",
            lambda: mint_pypi_token("x", lambda *a, **k: (422, {"errors": [{"code": "invalid-payload",
                                                                             "description": "malformed"}]})),
            "invalid-payload")
    expect("PyPI without the version", pypi_files("9.9.9", lambda url: (404, None)) == {})
    expect("PyPI with it", pypi_files("9.9.9", lambda url: (200, {"urls": [
        {"filename": "a.whl", "digests": {"sha256": "ab"}}]})) == {"a.whl": "ab"})
    expect("the Gallery's versions", gallery_versions(
        lambda url: "<entry><d:Version>6.3.6</d:Version></entry><entry><d:Version>6.3.5</d:Version></entry>")
        == {"6.3.5", "6.3.6"})

    # Staging a tree: the package's files, LF, hmctl byte for byte, the version.
    check = checker()
    work = tempfile.mkdtemp(prefix="hm-cli-packages-")
    try:
        tree = os.path.join(work, "tree")
        for relative in [check.VERSION_H, check.HMCTL, os.path.join(check.PYTHON_DIR, "pyproject.toml"),
                         os.path.join(check.PYTHON_DIR, "README.md"), "LICENSE", check.REQUIREMENTS]:
            os.makedirs(os.path.dirname(os.path.join(tree, relative)), exist_ok=True)
            shutil.copyfile(os.path.join(ROOT, relative), os.path.join(tree, relative))
        with open(os.path.join(tree, check.PYTHON_DIR, "README.md"), "ab") as f:
            f.write(b"a CRLF line\r\n")
        staged = os.path.join(work, "stage")
        version = stage(tree, staged)
        expect("the staged version is Version.h's", version == check.read_version(ROOT))
        with open(os.path.join(staged, MODULE, "hmctl.py"), "rb") as f, open(os.path.join(ROOT, check.HMCTL), "rb") as g:
            expect("hmctl.py is hmctl byte for byte", f.read() == g.read())
        with open(os.path.join(staged, "README.md"), "rb") as f:
            expect("the staged text files are LF", b"\r\n" not in f.read())
        with open(os.path.join(staged, MODULE, "__init__.py"), encoding="utf-8") as f:
            expect("__init__.py carries the version", '__version__ = "%s"' % version in f.read())
        expect("the staged package is the three modules",
               sorted(os.listdir(os.path.join(staged, MODULE))) == ["__init__.py", "__main__.py", "hmctl.py"])
        with open(os.path.join(tree, check.PYTHON_DIR, "setup.py"), "w") as f:
            f.write("import os\n")
        refused("a tree with a setup.py", lambda: stage(tree, staged), "setup.py")
        os.remove(os.path.join(tree, check.PYTHON_DIR, "setup.py"))
        shutil.rmtree(os.path.join(tree, check.PYTHON_DIR))
        refused("a tree without the package", lambda: stage(tree, staged), "the first tree with the package")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    publish_cases(expect, refused)

    print("")
    if failures:
        print("%d self-test case(s) failed" % len(failures))
        return 1
    print("Every self-test case passed.")
    return 0


class Answer:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def publish_cases(expect, refused):
    """The two publish commands end to end, with the release's tree made from this
    checkout as git archive makes it (TREE_PATHS and nothing else), and the feed,
    PyPI, the Gallery, git, twine, pypi-attestations and pwsh answered in memory:
    what is sent where, what is never on a command line, and when each job
    publishes nothing. The PyPI cases that build need build and flit_core (the
    jobs' environment has both)."""
    check = checker()
    version = check.read_version(ROOT)
    buildable = importlib.util.find_spec("build") is not None and importlib.util.find_spec("flit_core") is not None
    sdist = "%s-%s.tar.gz" % (MODULE, version)
    wheel = "%s-%s-py3-none-any.whl" % (MODULE, version)

    def tree_of(signed):
        def tree_fn(wanted, into, root=None, run=None):
            for relative in TREE_PATHS:
                source, target = os.path.join(ROOT, relative), os.path.join(into, relative)
                if os.path.isdir(source):
                    shutil.copytree(source, target, ignore=shutil.ignore_patterns("__pycache__"))
                else:
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    shutil.copyfile(source, target)
            if signed:
                with open(os.path.join(into, check.PSM1), "ab") as f:
                    f.write(b"\r\n" + SIGNATURE_BLOCK + b"\r\n# MIIxxx\r\n# SIG # End signature block\r\n")
            return "0" * 40
        return tree_fn

    tree_fn = tree_of(signed=False)

    def world(minted=None, held=None, project=None):
        """(fetch, run, calls): the feed, PyPI and git as the cases need them. project:
        what PyPI's JSON API answers for the project - 200 or 404, or by the files."""
        calls = []
        state = {"uploaded": {}}

        def fetch(url, data=None, headers=None):
            calls.append(("fetch", url, data))
            if url in (FEED_TAG % ("v" + version), FEED_LATEST):
                return 200, {"tag_name": "v" + version, "draft": False, "prerelease": False}
            if url.startswith(FEED):
                return 404, None
            if url.endswith("/_/oidc/mint-token"):
                if minted:
                    return 200, {"success": True, "token": minted}
                return 422, {"message": "Token request failed", "errors": [
                    {"code": "invalid-publisher", "description": "valid token, but no corresponding publisher"}]}
            files = held if held is not None else state["uploaded"]
            if url.endswith("/pypi/%s/json" % DIST_NAME):
                if project is not None:
                    return project, None
                return (200, {}) if files else (404, None)
            if "/pypi/%s/" % DIST_NAME in url:
                if not files:
                    return 404, None
                return 200, {"urls": [{"filename": n, "digests": {"sha256": d}} for n, d in files.items()]}
            if "/integrity/" in url:
                return 200, {}
            return 404, None

        def run(command, **kwargs):
            calls.append(("run", list(command), kwargs.get("env")))
            if command[:2] == ["git", "-C"] and "log" in command:
                return Answer(0, "1790000000\n")
            if "pypi_attestations" in command:
                for path in command[command.index("sign") + 1:]:
                    with open(path + ".publish.attestation", "w") as f:
                        f.write("{}")
                return Answer(0)
            if "twine" in command:
                for path in [c for c in command if c.endswith((".whl", ".tar.gz"))]:
                    (held if held is not None else state["uploaded"])[os.path.basename(path)] = sha256(path)
                return Answer(0)
            if command[0] == "pwsh":
                return Answer(0)
            return subprocess.run(command, **kwargs)

        return fetch, run, calls

    def quiet(call):
        held_stdout, sys.stdout = sys.stdout, io.StringIO()
        try:
            return call(), sys.stdout.getvalue()
        finally:
            sys.stdout = held_stdout

    work = tempfile.mkdtemp(prefix="hm-cli-publish-")
    try:
        base = {"HM_CHANNEL_VERSION": version, "CI_PROJECT_PATH": "hmailserver/hmailserver"}
        fetch, run, calls = world()
        code, said = quiet(lambda: pypi_publish(os.path.join(work, "p1"), dict(base, PYPI_ID_TOKEN="id.x.y",
                                                                                SIGSTORE_ID_TOKEN="id.s.t"),
                                                 fetch, run, tree_fn=tree_fn))
        expect("PyPI without a trusted publisher or the project: 'not configured', exit 0, before any release is"
               " read or built",
               code == 0 and "not configured" in said and "namespace hmailserver, project hmailserver" in said
               and not any(c[0] == "run" for c in calls)
               and [c[1] for c in calls if c[0] == "fetch" and "/pypi/" in c[1]] == [
                   "%s/pypi/%s/json" % (PYPI, DIST_NAME)])
        fetch, run, calls = world()
        code, said = quiet(lambda: pypi_publish(os.path.join(work, "p1b"), dict(PYPI_ID_TOKEN="id.x.y"),
                                                 fetch, run, tree_fn=tree_fn))
        expect("PyPI without a trusted publisher, whatever the feed says: still 'not configured', exit 0",
               code == 0 and "not configured" in said and not any(c[1].startswith(FEED) for c in calls))
        fetch, run, calls = world(project=200)
        refused("PyPI with the project and no publisher that matches this pipeline any more: the job fails",
                lambda: quiet(lambda: pypi_publish(os.path.join(work, "p1c"), dict(base, PYPI_ID_TOKEN="id.x.y"),
                                                   fetch, run, tree_fn=tree_fn)),
                "none of its trusted publishers matches")
        fetch, run, calls = world(project=503)
        refused("PyPI with no publisher and its JSON API down: the job fails, not 'not configured'",
                lambda: quiet(lambda: pypi_publish(os.path.join(work, "p1d"), dict(base, PYPI_ID_TOKEN="id.x.y"),
                                                   fetch, run, tree_fn=tree_fn)),
                "answered 503")

        fetch, run, calls = world(minted="pypi-x", held={sdist: "ab", wheel: "cd"})
        code, said = quiet(lambda: pypi_publish(os.path.join(work, "p2"), dict(base, PYPI_ID_TOKEN="id.x.y"),
                                                 fetch, run, tree_fn=tree_fn))
        expect("PyPI with both files of the version already: exit 0, nothing built, signed or sent",
               code == 0 and "Nothing to publish" in said and not any(c[0] == "run" for c in calls))
        refused("PyPI without the job's ID token",
                lambda: quiet(lambda: pypi_publish(os.path.join(work, "p4"), dict(base), world()[0], world()[1],
                                                   tree_fn=tree_fn)), "no PYPI_ID_TOKEN")

        if not buildable:
            print("skip  the PyPI cases that publish: they build the package, and build and flit_core are not"
                  " installed here (the jobs install both)")
        else:
            secret = "pypi-AgEIcHlwaS5vcmcCJGNhZmU"
            fetch, run, calls = world(minted=secret)
            code, said = quiet(lambda: pypi_publish(os.path.join(work, "p3"), dict(base, PYPI_ID_TOKEN="id.x.y",
                                                                                    SIGSTORE_ID_TOKEN="id.s.t"),
                                                     fetch, run, tree_fn=tree_fn))
            uploads = [c for c in calls if c[0] == "run" and "twine" in c[1]]
            expect("PyPI with a publisher: the release tree as git archive makes it builds; signed, then uploaded"
                   " with --attestations and each file's attestation, the token in twine's environment",
                   code == 0 and len(uploads) == 1 and "--attestations" in uploads[0][1]
                   and len([c for c in uploads[0][1] if c.endswith(".publish.attestation")]) == 2
                   and uploads[0][2].get("TWINE_PASSWORD") == secret and uploads[0][2].get("TWINE_USERNAME") == "__token__"
                   and any(c[0] == "run" and "pypi_attestations" in c[1] for c in calls)
                   and "Published" in said)
            expect("the PyPI token is never on a command line or in the output",
                   not any(secret in " ".join(c[1]) for c in calls if c[0] == "run") and secret not in said)
            dated = [c for c in calls if c[0] == "run" and c[1][:2] == ["git", "-C"] and "log" in c[1]]
            expect("the build is dated by the tag's commit", len(dated) == 1 and ("0" * 40) in dated[0][1])

            partial = {wheel: "cd"}
            fetch, run, calls = world(minted=secret, held=partial)
            code, said = quiet(lambda: pypi_publish(os.path.join(work, "p5"), dict(base, PYPI_ID_TOKEN="id.x.y",
                                                                                    SIGSTORE_ID_TOKEN="id.s.t"),
                                                     fetch, run, tree_fn=tree_fn))
            uploads = [c for c in calls if c[0] == "run" and "twine" in c[1]]
            signs = [c for c in calls if c[0] == "run" and "pypi_attestations" in c[1]]
            sent = [os.path.basename(c) for c in uploads[0][1] if c.endswith((".whl", ".tar.gz"))] if uploads else []
            expect("PyPI with one of the two files, an upload stopped part-way: the other alone is signed and sent,"
                   " and the one PyPI has is left as it is",
                   code == 0 and sent == [sdist] and len(signs) == 1
                   and [os.path.basename(p) for p in signs[0][1] if p.endswith((".whl", ".tar.gz"))] == [sdist]
                   and "stopped part-way" in said and "is other bytes" in said and partial[wheel] == "cd")

        fetch, run, calls = world()
        code, said = quiet(lambda: psgallery_publish(os.path.join(work, "g1"), dict(base), fetch, run,
                                                      fetch_text=lambda url: "", tree_fn=tree_fn))
        expect("the Gallery without PSGALLERY_API_KEY: 'not configured', exit 0, before any release is read",
               code == 0 and "not configured" in said and not calls
               and not os.path.exists(os.path.join(work, "g1", "tree")))
        key = "oy2kbGalleryKeyValue7q"
        fetch, run, calls = world()
        code, said = quiet(lambda: psgallery_publish(os.path.join(work, "g2"), dict(base, PSGALLERY_API_KEY=key), fetch,
                                                      run, fetch_text=lambda url: "", tree_fn=tree_of(signed=True)))
        pwsh = [c for c in calls if c[0] == "run" and c[1][0] == "pwsh"]
        expect("the Gallery with the key and a signed module: Publish-PSResource of the module's folder, the key by"
               " name alone",
               code == 0 and len(pwsh) == 1 and "Publish-PSResource" in pwsh[0][1][-1]
               and "$env:PSGALLERY_API_KEY" in pwsh[0][1][-1] and key not in " ".join(pwsh[0][1]) and key not in said
               and sorted(os.listdir(os.path.join(work, "g2", "module", GALLERY_NAME))) == ["HmailServer.psd1",
                                                                                           "HmailServer.psm1"])
        fetch, run, calls = world()
        refused("the Gallery with the key and the tag's module unsigned: refused, nothing sent",
                lambda: quiet(lambda: psgallery_publish(os.path.join(work, "g2b"), dict(base, PSGALLERY_API_KEY=key),
                                                        fetch, run, fetch_text=lambda url: "", tree_fn=tree_fn)),
                "carries no Authenticode signature")
        expect("  ... and pwsh never ran", not any(c[0] == "run" and c[1][0] == "pwsh" for c in calls))
        fetch, run, calls = world()
        code, said = quiet(lambda: psgallery_publish(os.path.join(work, "g3"), dict(base, PSGALLERY_API_KEY=key), fetch,
                                                      run, fetch_text=lambda url: "<d:Version>%s</d:Version>" % version,
                                                      tree_fn=tree_fn))
        expect("the Gallery with the version already: exit 0, nothing read from the tag, no pwsh",
               code == 0 and "already" in said and not any(c[0] == "run" for c in calls)
               and not os.path.exists(os.path.join(work, "g3", "tree")))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--self-test", action="store_true")
    commands = parser.add_subparsers(dest="command")
    b = commands.add_parser("build", help="the sdist and the wheel, from a tree")
    b.add_argument("--tree", default=ROOT)
    b.add_argument("--out", default="cli-packages")
    b.add_argument("--isolation", action="store_true",
                   help="let python -m build fetch the backend itself (the jobs install it hashed and do not)")
    for name in ("pypi-publish", "psgallery-publish"):
        p = commands.add_parser(name)
        p.add_argument("--out", default="cli-publish")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    try:
        if arguments.command == "build":
            version, _ = build(arguments.tree, arguments.out, isolation=arguments.isolation)
            print("%s %s: sdist and wheel in %s" % (DIST_NAME, version, os.path.join(arguments.out, "dist")))
            return 0
        if arguments.command == "pypi-publish":
            return pypi_publish(os.path.abspath(arguments.out))
        if arguments.command == "psgallery-publish":
            return psgallery_publish(os.path.abspath(arguments.out))
    except Refused as error:
        print("REFUSED: %s" % error)
        return 1
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
