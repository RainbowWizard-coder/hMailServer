#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The release a channel publishes, and its files verified before anything is sent
(.gitlab/ci/channels.yml; hmailserver/docs/Channels.md). Every channel job loads this.

A channel job publishes a RELEASE, never the commit its pipeline is for, and runs from
master whenever its channel's account exists - weeks after the tag, if that is when
the account arrives - with no new release made for it:

  the release   the project's CI/CD variable HM_CHANNEL_VERSION when it is set, and
                otherwise the update feed's releases/latest; either way the feed must
                list it as a published release, not a draft or a pre-release. This is
                build/ci/cli-packages.py's release_version, loaded from there, so every
                channel and the two command-line packages take the same release by the
                same rule. A version is never given when the job is started: the
                project refuses pipeline variables.

  its files     read from the GitLab release v<version>, anonymously (the project is
                public): each file's link must be the generic package registry's
                address build/ci/release-chain.py gave it, and each file is downloaded
                with <file>.cosign.bundle beside it.

  verified      each file with cosign verify-blob against its bundle, as signed by
                this project's pipeline for that tag - at either of the two paths a
                server trusts, Progressiverobot/hmailserver and hmailserver/hmailserver,
                since a release signed before the project's move may be published
                after it - and with the project's id (86730559) in the certificate
                required at the group's path and refused if another, as the server
                requires (SigstoreVerifier.cpp); the id read from the certificate's
                extensions by build/ci/sigstore-certificate.py, as container-image-deb.sh
                reads it in shell. A file that does not verify stops the job before
                anything is published anywhere.

  the tools     cosign and regctl, each a static binary taken out of its own image by
                this script: cosign from HM_COSIGN_IMAGE (gcr.io, the release chain's,
                pinned by digest in .gitlab-ci.yml), regctl from HM_REGCTL_IMAGE (Docker
                Hub's regclient/regctl, pinned by digest in .gitlab/ci/channels.yml).
                Every manifest and layer is held to its digest, so the bytes are the
                pinned image's whatever the network. Nothing comes from GitHub.

Usage, from the root of a checkout:
  python3 build/ci/channels/release-files.py verify [--out DIR] [FILE ...]   (FILE may say {version})
        the release's files the channels take - or those named - downloaded and
        verified, and verification.json written: what the channels-verify job does,
        and what a maintainer runs before an account exists
  python3 build/ci/channels/release-files.py fetch-tool cosign|regctl --into DIR
  python3 build/ci/channels/release-files.py --self-test [--cosign PATH]

--self-test runs offline against a feed, a release and a registry in memory. With
--cosign (a cosign 2.6 binary) and the cryptography package it also proves the
verification with that cosign against bundles build/ci/channels/fake-sigstore.py makes.

HM_CHANNELS_FIXTURE, outside CI only, names a JSON file that points the feed, the
release API and the registry at a local server and hands cosign a trusted root of its
own (fake-sigstore.py's): how each channel job is run against a fixture release on a
laptop. Any job refuses it when CI is set, so no CI/CD variable can point a channel at
another feed or trust.
"""

import argparse
import gzip
import hashlib
import importlib.util
import io
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
# build/ci/channels/ -> the root of the checkout.
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))

PROJECT_ID = "86730559"
API = "https://gitlab.com/api/v4/projects/" + PROJECT_ID
PACKAGE = "hmailserver"
ISSUER = "https://gitlab.com"
# The two paths a server trusts a release's signature from (SigstoreVerifier.cpp): the
# personal namespace the project began in, and the group it moves to, where the
# project's id must be in the certificate too.
REPOSITORIES = ("https://gitlab.com/Progressiverobot/hmailserver", "https://gitlab.com/hmailserver/hmailserver")
GROUP_REPOSITORY = REPOSITORIES[1]
USER_AGENT = "hmailserver-channels (+https://gitlab.com/hmailserver/hmailserver)"
BUNDLE = ".cosign.bundle"

# The pinned images the two tools come out of. Each is read from the job's variable
# when it is set and from the file that pins it otherwise (the Copr builder runs this
# outside GitLab), and must be pinned by digest either way.
TOOLS = {
    "cosign": {"variable": "HM_COSIGN_IMAGE", "file": ".gitlab-ci.yml", "member": "ko-app/cosign",
               "pattern": re.compile(r"^gcr\.io/projectsigstore/cosign:[A-Za-z0-9._\-]+@sha256:[0-9a-f]{64}$")},
    "regctl": {"variable": "HM_REGCTL_IMAGE", "file": os.path.join(".gitlab", "ci", "channels.yml"), "member": "regctl",
               "pattern": re.compile(r"^docker\.io/regclient/regctl:v[0-9][A-Za-z0-9._\-]*@sha256:[0-9a-f]{64}$")},
}


def load(name, relative):
    """A module of this repository by its path (the scripts' names have hyphens), once
    per process: the channel scripts that load this one and each other share one
    cli-packages.py, and so one Refused."""
    if name in sys.modules:
        return sys.modules[name]
    path = os.path.join(ROOT, relative)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


# The release rule, and the stop that is the operator's to act on, are
# cli-packages.py's: one rule for every channel.
cli = load("cli_packages", os.path.join("build", "ci", "cli-packages.py"))
Refused = cli.Refused


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def say(line=""):
    print(line)
    sys.stdout.flush()


# ---------------------------------------------------------------- a fixture, outside CI only

class Fixture:
    """Where a laptop run of a channel job looks instead of the real feed, release and
    trust: {"feed": URL, "api": URL, "trusted_root": PATH, and anything a channel's
    script reads for its own target}. None in CI, and a job refuses the variable there."""

    def __init__(self, data):
        self.data = data

    def get(self, key, default=None):
        return self.data.get(key, default)

    def rebase(self, url):
        feed = self.data.get("feed")
        api = self.data.get("api")
        if feed and url.startswith(cli.FEED):
            return feed.rstrip("/") + "/" + url[len(cli.FEED):]
        if api and url.startswith(API):
            return api.rstrip("/") + url[len(API):]
        return url


def fixture(environ=None):
    environ = os.environ if environ is None else environ
    path = environ.get("HM_CHANNELS_FIXTURE")
    if not path:
        return None
    if environ.get("CI") or environ.get("GITLAB_CI"):
        raise Refused("HM_CHANNELS_FIXTURE is set in a CI job: a channel job never takes another feed, release or trust"
                      " than the project's own. Remove the variable.")
    with open(path, encoding="utf-8") as f:
        return Fixture(json.load(f))


# ---------------------------------------------------------------- HTTP

def http(url, data=None, headers=None, method=None, timeout=120, fix=None):
    """(status, headers with lower-case names, body) of one request; redirects followed."""
    if fix is not None:
        url = fix.rebase(url)
    request = urllib.request.Request(url, data=data, method=method,
                                     headers=dict({"User-Agent": USER_AGENT}, **(headers or {})))
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, {k.lower(): v for k, v in response.headers.items()}, response.read()
    except urllib.error.HTTPError as error:
        return error.code, {k.lower(): v for k, v in error.headers.items()}, error.read()


def http_json(url, fix=None, data=None, headers=None):
    status, _, body = http(url, data=data, headers=dict({"Accept": "application/json"}, **(headers or {})), fix=fix)
    try:
        return status, json.loads(body.decode("utf-8")) if body else None
    except ValueError:
        return status, None


def download(url, path, fix=None, tries=3):
    """The file at url, anonymously, into path; three tries."""
    last = None
    for attempt in range(tries):
        status, _, body = http(url, fix=fix, timeout=600)
        if status == 200:
            with open(path, "wb") as f:
                f.write(body)
            return
        last = status
        if status == 404:
            break
        time.sleep(5 * (attempt + 1))
    raise Refused("%s answered %s" % (url, last))


# ---------------------------------------------------------------- the release

def release_version(environ=None, fix=None):
    """(version, where it came from): HM_CHANNEL_VERSION or the feed's latest, and the
    feed must list it as a published release (cli-packages.py's rule). The version
    names files and paths here, so it is held to ASCII digits and two dots once more:
    Python's \\d and $ also take other scripts' digits and a final newline."""
    version, where = cli.release_version(environ, lambda url, **kw: http_json(url, fix=fix))
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise Refused("%s names %s: the channels take a release version, X.Y.Z" % (where, ascii(version)))
    return version, where


def registry_url(version, name):
    return "%s/packages/generic/%s/%s/%s" % (API, PACKAGE, urllib.parse.quote(version, safe=""),
                                             urllib.parse.quote(name, safe=""))


def release_links(version, fix=None):
    """{name: url} of the GitLab release v<version>'s links, each where
    build/ci/release-chain.py puts a file."""
    tag = "v" + version
    status, answer = http_json("%s/releases/%s" % (API, urllib.parse.quote(tag, safe="")), fix=fix)
    if status != 200 or not isinstance(answer, dict) or answer.get("tag_name") != tag:
        raise Refused("the GitLab release %s could not be read (HTTP %s): it is not published" % (tag, status))
    links = {}
    for link in (answer.get("assets") or {}).get("links") or []:
        if isinstance(link, dict) and link.get("name"):
            links[link["name"]] = link.get("url") or ""
    return links


def identities(version):
    return [repository + "//.gitlab-ci.yml@refs/tags/v" + version for repository in REPOSITORIES]


# ---------------------------------------------------------------- the certificate's repository id

# Read from the certificate's extensions by build/ci/sigstore-certificate.py, the one
# reader of the release chain and the channels (container-image-deb.sh's shell reader
# is held to it): never by a search of the certificate's bytes, which its public key,
# bytes its holder chooses, could fill with this project's id.
sigstore_certificate = load("sigstore_certificate", os.path.join("build", "ci", "sigstore-certificate.py"))
REPOSITORY_ID_OID = sigstore_certificate.REPOSITORY_ID_OID
der = sigstore_certificate.der
repository_id = sigstore_certificate.repository_id
bundle_certificate = sigstore_certificate.bundle_certificate
test_certificate = sigstore_certificate.test_certificate


# ---------------------------------------------------------------- cosign

def cosign_verify(cosign, path, bundle_path, identity, trusted_root=None, run=subprocess.run):
    """True when cosign verify-blob verifies path against its bundle as identity."""
    command = [cosign, "verify-blob", "--new-bundle-format", "--bundle", bundle_path,
               "--certificate-identity", identity, "--certificate-oidc-issuer", ISSUER]
    if trusted_root:
        command[2:2] = ["--trusted-root", trusted_root]
    answer = run(command + [path], capture_output=True, text=True)
    return answer.returncode == 0, ((answer.stderr or "") + (answer.stdout or "")).strip()


def verify_file(path, bundle_path, version, cosign, trusted_root=None, run=subprocess.run):
    """(identity, repository id) of a file that verifies as this project's pipeline
    for v<version>; Refused otherwise."""
    name = os.path.basename(path)
    said = ""
    identity = None
    for candidate in identities(version):
        ok, said = cosign_verify(cosign, path, bundle_path, candidate, trusted_root, run)
        if ok:
            identity = candidate
            break
    if identity is None:
        raise Refused("%s does not verify as signed by this project's pipeline for v%s (%s): nothing was published.\n"
                      "  cosign said: %s" % (name, version, " or ".join(identities(version)),
                                            said.splitlines()[-1] if said else "nothing"))
    try:
        recorded = repository_id(sigstore_certificate.read_bundle(bundle_path))
    except ValueError as error:
        raise Refused("%s's bundle verified, but its certificate could not be read for the project's id: %s" % (name, error))
    if recorded is not None and recorded != PROJECT_ID:
        raise Refused("%s's certificate names the repository id %s, not %s: another project's pipeline signed it"
                      % (name, recorded, PROJECT_ID))
    if recorded is None and identity.startswith(GROUP_REPOSITORY + "//"):
        raise Refused("%s's certificate, at the group's path, records no repository id: a server refuses it, and so"
                      " does this job" % name)
    return identity, recorded


def verified_files(version, names, into, cosign, fix=None, run=subprocess.run):
    """Each named file of the release v<version>, downloaded into `into` with its
    bundle and verified. Answers {name: {path, sha256, size, identity, repository_id}};
    Refused at the first that is missing or does not verify."""
    links = release_links(version, fix)
    os.makedirs(into, exist_ok=True)
    trusted_root = fix.get("trusted_root") if fix else None
    record = {}
    signed_as = None
    for name in names:
        for wanted in (name, name + BUNDLE):
            url = registry_url(version, wanted)
            if links.get(wanted) != url:
                raise Refused("the release v%s has no link to %s: it does not carry %s%s" % (
                    version, url, wanted, "" if wanted not in links else " there (its link is %s)" % links[wanted]))
        path = os.path.join(into, name)
        download(registry_url(version, name), path, fix)
        download(registry_url(version, name + BUNDLE), path + BUNDLE, fix)
        if os.path.getsize(path) == 0:
            raise Refused("%s is empty" % name)
        identity, recorded = verify_file(path, path + BUNDLE, version, cosign, trusted_root, run)
        if signed_as is not None and identity != signed_as:
            raise Refused("%s is signed as %s, and the file before it as %s" % (name, identity, signed_as))
        signed_as = identity
        digest = sha256_file(path)
        record[name] = {"path": path, "sha256": digest, "size": os.path.getsize(path), "identity": identity,
                        "repository_id": recorded or "none recorded"}
        say("%s  %s  verifies as %s (repository id: %s)" % (name, digest, identity, recorded or "none recorded"))
    return record


def write_record(record, version, where, path):
    """verification.json: what was verified, for the log, the job's artifact and a
    later job of the same pipeline."""
    document = {"version": version, "tag": "v" + version, "from": where, "issuer": ISSUER,
                "files": dict((n, dict((k, v) for k, v in e.items() if k != "path")) for n, e in record.items())}
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(document, f, indent=1, sort_keys=True)
        f.write("\n")
    return document


# ---------------------------------------------------------------- the tools, out of their images

def pinned_image(tool, environ=None, root=ROOT):
    environ = os.environ if environ is None else environ
    spec = TOOLS[tool]
    value = (environ.get(spec["variable"]) or "").strip()
    if not value:
        path = os.path.join(root, spec["file"])
        try:
            with open(path, encoding="utf-8") as f:
                text = f.read()
        except OSError:
            text = ""
        found = re.search(r'^\s*%s:\s*"?([^"\s]+)"?\s*$' % spec["variable"], text, re.MULTILINE)
        value = found.group(1) if found else ""
    if not spec["pattern"].match(value):
        raise Refused("%s is %r: %s comes only from its own image pinned by digest (%s)" % (
            spec["variable"], value, tool, spec["pattern"].pattern))
    return value


def split_image(reference):
    """(registry host, repository, digest) of name[:tag]@sha256:..."""
    name, digest = reference.split("@", 1)
    host, _, path = name.partition("/")
    repository = path.rsplit(":", 1)[0] if ":" in path.rsplit("/", 1)[-1] else path
    if host == "docker.io":
        host = "registry-1.docker.io"
        if "/" not in repository:
            repository = "library/" + repository
    return host, repository, digest


MANIFEST_TYPES = ", ".join([
    "application/vnd.oci.image.index.v1+json", "application/vnd.docker.distribution.manifest.list.v2+json",
    "application/vnd.oci.image.manifest.v1+json", "application/vnd.docker.distribution.manifest.v2+json"])


def registry_get(host, repository, path, token, accept):
    headers = {"Accept": accept}
    request = urllib.request.Request("https://%s/v2/%s/%s" % (host, repository, path),
                                     headers=dict({"User-Agent": USER_AGENT}, **headers))
    if token:
        # Not carried to the storage a blob is redirected to, which refuses it.
        request.add_unredirected_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(request, timeout=300) as response:
            return response.status, {k.lower(): v for k, v in response.headers.items()}, response.read()
    except urllib.error.HTTPError as error:
        return error.code, {k.lower(): v for k, v in error.headers.items()}, error.read()


def anonymous_token(challenge, repository):
    fields = dict(re.findall(r'(\w+)="([^"]*)"', challenge or ""))
    if not fields.get("realm", "").startswith("https://"):
        raise Refused("the registry asked for a token from %r" % fields.get("realm"))
    url = "%s?%s" % (fields["realm"], urllib.parse.urlencode(
        [("service", fields.get("service", "")), ("scope", "repository:%s:pull" % repository)]))
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": USER_AGENT}), timeout=60) as response:
        answer = json.load(response)
    return answer.get("token") or answer.get("access_token")


def fetch_tool(tool, into, environ=None, get=registry_get, machine=None):
    """The tool's binary out of its pinned image, into `into`: every manifest and layer
    held to its digest. Answers the path."""
    reference = pinned_image(tool, environ)
    host, repository, digest = split_image(reference)
    arch = {"x86_64": "amd64", "amd64": "amd64", "aarch64": "arm64", "arm64": "arm64"}.get(
        (machine or platform.machine()).lower())
    if not arch:
        raise Refused("no %s for this machine (%s)" % (tool, machine or platform.machine()))
    token = None

    def fetch(path, accept, want):
        nonlocal token
        status, headers, body = get(host, repository, path, token, accept)
        if status == 401 and token is None:
            token = anonymous_token(headers.get("www-authenticate"), repository)
            status, headers, body = get(host, repository, path, token, accept)
        if status != 200:
            raise Refused("%s answered %s for %s/%s" % (host, status, repository, path))
        if "sha256:" + hashlib.sha256(body).hexdigest() != want:
            raise Refused("%s/%s from %s is not %s: refused" % (repository, path, host, want))
        return body

    index = json.loads(fetch("manifests/" + digest, MANIFEST_TYPES, digest))
    manifest = index
    if "manifests" in index:
        chosen = [m for m in index["manifests"] if (m.get("platform") or {}).get("os") == "linux"
                  and (m.get("platform") or {}).get("architecture") == arch]
        if not chosen:
            raise Refused("%s has no linux/%s image" % (reference, arch))
        manifest = json.loads(fetch("manifests/" + chosen[0]["digest"], MANIFEST_TYPES, chosen[0]["digest"]))
    member = TOOLS[tool]["member"]
    data = None
    for layer in reversed(manifest.get("layers") or []):
        blob = fetch("blobs/" + layer["digest"], "*/*", layer["digest"])
        opener = gzip.open if blob[:2] == b"\x1f\x8b" else None
        stream = io.BytesIO(gzip.decompress(blob) if opener else blob)
        with tarfile.open(fileobj=stream, mode="r:") as archive:
            for info in archive.getmembers():
                if info.name.lstrip("./") == member and info.isreg():
                    data = archive.extractfile(info).read()
        if data is not None:
            break
    if data is None:
        raise Refused("%s holds no %s" % (reference, member))
    os.makedirs(into, exist_ok=True)
    path = os.path.join(into, tool)
    with open(path, "wb") as f:
        f.write(data)
    # Executable by the job's own user, who alone runs it.
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)
    say("%s: %s from %s (SHA-256 %s)" % (tool, path, reference, hashlib.sha256(data).hexdigest()))
    return path


def tool_path(tool, work, environ=None):
    """The tool a job runs: HM_CHANNELS_<TOOL> outside CI (a laptop's own, for a
    fixture run), and otherwise the one out of its pinned image."""
    environ = os.environ if environ is None else environ
    given = environ.get("HM_CHANNELS_" + tool.upper())
    if given:
        if environ.get("CI") or environ.get("GITLAB_CI"):
            raise Refused("HM_CHANNELS_%s is set in a CI job: the job runs the %s of its pinned image" % (tool.upper(), tool))
        return given
    return fetch_tool(tool, os.path.join(work, "tools"), environ)


# ---------------------------------------------------------------- what the channels take

def channel_files(version):
    """The release's files one channel or another publishes from: the source tarball
    (the AUR, the PPA, Copr and OBS build from it) and the Windows installer
    (Chocolatey). The image is verified in the registry (registry-mirror.py)."""
    return ["hmailserver-%s.tar.gz" % version, "hMailServer-%s-x64.exe" % version]


def verify_command(out, names, environ=None):
    fix = fixture(environ)
    version, where = release_version(environ, fix)
    say("the release: %s (from %s)" % (version, where))
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    cosign = tool_path("cosign", out, environ)
    # A name may say {version}: the chocolatey-release job names the installer so.
    names = [n.replace("{version}", version) for n in names] if names else channel_files(version)
    record = verified_files(version, names, os.path.join(out, "files"), cosign, fix)
    write_record(record, version, where, os.path.join(out, "verification.json"))
    say("VERIFIED: %d file(s) of v%s, each as this project's pipeline for its tag; nothing was published."
        % (len(record), version))
    return 0


# ---------------------------------------------------------------- self-test

class Answer:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def self_test(cosign=None):
    failures = []

    def expect(what, condition):
        print("%s  %s" % ("ok  " if condition else "FAIL", what))
        if not condition:
            failures.append(what)

    def refused(what, call, needle):
        try:
            call()
        except Refused as error:
            expect("%s (%s)" % (what, str(error).splitlines()[0][:110]), needle in str(error))
            return
        expect(what + ": not refused", False)

    # The repository id, out of certificates made by hand around the extension.
    def extension(value, critical=False):
        return test_certificate(value, critical=critical)
    expect("the repository id is read out of a certificate", repository_id(extension("86730559")) == "86730559")
    expect("  ... and out of a critical extension", repository_id(extension("1267388815", True)) == "1267388815")
    expect("  ... and none where the extension is not", repository_id(test_certificate()) is None)
    # The extension's bytes put into the public key, which its holder chooses (an RSA
    # modulus), in front of the real extension: only the extension counts.
    spoof = REPOSITORY_ID_OID + der(0x04, der(0x0C, b"86730559"))
    expect("  ... the id in the extensions, not this project's id planted in the public key before them",
           repository_id(test_certificate("1267388815", key=b"\x00" * 7 + spoof)) == "1267388815")
    expect("  ... and none recorded, however the key's bytes read", repository_id(test_certificate(key=spoof)) is None)
    for what, broken in (("the extension twice", test_certificate("86730559", twice=True)),
                         ("a certificate cut short", test_certificate("86730559")[:-3]),
                         ("not a certificate", b"\x04\x02ab")):
        try:
            repository_id(broken)
            expect("  ... %s: refused" % what, False)
        except ValueError:
            expect("  ... %s: refused" % what, True)

    # The pins.
    expect("cosign's pin is read from .gitlab-ci.yml", pinned_image("cosign", {}).startswith("gcr.io/projectsigstore/cosign:"))
    refused("a cosign image not pinned by digest", lambda: pinned_image("cosign", {"HM_COSIGN_IMAGE": "gcr.io/projectsigstore/cosign:v2"}),
            "pinned by digest")
    refused("a cosign image from elsewhere", lambda: pinned_image("cosign", {
        "HM_COSIGN_IMAGE": "ghcr.io/sigstore/cosign/cosign:v2.6.5@sha256:" + "a" * 64}), "pinned by digest")
    expect("an image reference split", split_image("docker.io/regclient/regctl:v0.11.6@sha256:" + "b" * 64)
           == ("registry-1.docker.io", "regclient/regctl", "sha256:" + "b" * 64))
    expect("  ... and gcr.io's", split_image("gcr.io/projectsigstore/cosign:v2.6.5-dev@sha256:" + "c" * 64)
           == ("gcr.io", "projectsigstore/cosign", "sha256:" + "c" * 64))

    # A tool out of an image in memory: an index, a manifest, two layers, the binary
    # in the upper one; then the same with a layer that is not its digest.
    binary = b"\x7fELF fake cosign"
    lower, upper = io.BytesIO(), io.BytesIO()
    for stream, members in ((lower, {"etc/passwd": b"root"}), (upper, {"ko-app/cosign": binary})):
        with tarfile.open(fileobj=stream, mode="w:gz") as archive:
            for name, data in members.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
    blobs = {}

    def put(data):
        digest = "sha256:" + hashlib.sha256(data).hexdigest()
        blobs[digest] = data
        return digest
    layers = [{"digest": put(lower.getvalue())}, {"digest": put(upper.getvalue())}]
    manifest = json.dumps({"layers": layers}).encode()
    index = json.dumps({"manifests": [{"digest": put(manifest), "platform": {"os": "linux", "architecture": "amd64"}}]}).encode()
    index_digest = put(index)
    asked = []

    def registry(host, repository, path, token, accept):
        asked.append((host, repository, path, token))
        if token is None:
            return 401, {"www-authenticate": 'Bearer realm="https://auth.example/token",service="x"'}, b""
        return (200, {}, blobs[path.split("/", 1)[1]]) if path.split("/", 1)[1] in blobs else (404, {}, b"")
    held_token = globals()["anonymous_token"]
    globals()["anonymous_token"] = lambda challenge, repository: "t0ken"
    work = tempfile.mkdtemp(prefix="hm-channels-")
    try:
        environ = {"HM_COSIGN_IMAGE": "gcr.io/projectsigstore/cosign:v2.6.5-dev@" + index_digest}
        path = fetch_tool("cosign", work, environ, registry, machine="x86_64")
        with open(path, "rb") as f:
            expect("the tool is taken out of the upper layer, after a token", f.read() == binary and asked[1][3] == "t0ken")
        blobs[layers[1]["digest"]] = upper.getvalue() + b"tampered"
        refused("a layer that is not its digest", lambda: fetch_tool("cosign", work, environ, registry, machine="x86_64"),
                "is not sha256:")
        refused("no image for the machine", lambda: fetch_tool("cosign", work, environ, registry, machine="s390x"), "no cosign")
    finally:
        globals()["anonymous_token"] = held_token
        shutil.rmtree(work, ignore_errors=True)

    # The release's version, as the feed gives it: digits and dots and nothing else.
    held_json = globals()["http_json"]
    try:
        for tag in ("v9.9.9\n", "v٩.٩.٩", "v9.9.9"):
            globals()["http_json"] = lambda url, fix=None, data=None, headers=None, tag=tag: (
                200, {"tag_name": tag, "draft": False, "prerelease": False})
            if tag == "v9.9.9":
                expect("  ... and v9.9.9 taken", release_version({}) == ("9.9.9", cli.FEED_LATEST))
            else:
                refused("a feed whose latest is %s" % ascii(tag), lambda: release_version({}), "X.Y.Z")
    finally:
        globals()["http_json"] = held_json

    # The fixture variable: taken outside CI, refused in it.
    refused("HM_CHANNELS_FIXTURE in a CI job", lambda: fixture({"HM_CHANNELS_FIXTURE": "x.json", "CI": "true"}), "in a CI job")
    refused("HM_CHANNELS_COSIGN in a CI job", lambda: tool_path("cosign", ".", {"HM_CHANNELS_COSIGN": "/x", "GITLAB_CI": "true"}),
            "in a CI job")
    fix = Fixture({"feed": "http://127.0.0.1:1/feed", "api": "http://127.0.0.1:1/api"})
    expect("a fixture rebases the feed and the API, and nothing else",
           fix.rebase(cli.FEED_LATEST) == "http://127.0.0.1:1/feed/latest"
           and fix.rebase(API + "/releases/v1.2.3") == "http://127.0.0.1:1/api/releases/v1.2.3"
           and fix.rebase("https://aur.archlinux.org/x") == "https://aur.archlinux.org/x")

    # verified_files against a release and a registry in memory, cosign answered by
    # identity: which identities are tried, in which order, and what stops the job.
    version = "9.9.9"
    names = channel_files(version)
    work = tempfile.mkdtemp(prefix="hm-channels-")
    try:
        files = dict((n, ("content of %s" % n).encode()) for n in names)

        def bundle_for(repository_id, key=b""):
            import base64
            cert = test_certificate(repository_id, key=key)
            return json.dumps({"verificationMaterial": {"certificate": {"rawBytes": base64.b64encode(cert).decode()}}}).encode()

        def world(signer, repository_id="86730559", links=None, missing=None, key=b""):
            state = {"verify": []}
            served = dict(files)
            served.update(dict((n + BUNDLE, bundle_for(repository_id, key)) for n in names))

            def fake_http(url, data=None, headers=None, method=None, timeout=120, fix=None):
                if url.endswith("/releases/v" + version):
                    listed = links if links is not None else dict((n, registry_url(version, n)) for n in served)
                    body = {"tag_name": "v" + version, "assets": {"links": [{"name": n, "url": u} for n, u in listed.items()]}}
                    return 200, {}, json.dumps(body).encode()
                for name, data in served.items():
                    if url == registry_url(version, name) and name != missing:
                        return 200, {}, data
                return 404, {}, b""

            def run(command, **kwargs):
                identity = command[command.index("--certificate-identity") + 1]
                state["verify"].append(identity)
                return Answer(0 if identity == signer else 1, stderr="no matching CertificateIdentity")
            return fake_http, run, state

        held_http = globals()["http"]
        try:
            fake_http, run, state = world(identities(version)[1])
            globals()["http"] = fake_http
            record = verified_files(version, names, os.path.join(work, "a"), "cosign", run=run)
            expect("every file verified at the group's path, the personal path tried first, the record kept",
                   sorted(record) == sorted(names) and state["verify"] == [identities(version)[0], identities(version)[1]] * 2
                   and record[names[0]]["repository_id"] == "86730559"
                   and record[names[0]]["sha256"] == hashlib.sha256(files[names[0]]).hexdigest())
            fake_http, run, state = world(identities(version)[0], repository_id=None)
            globals()["http"] = fake_http
            record = verified_files(version, names, os.path.join(work, "b"), "cosign", run=run)
            expect("a release signed at the personal path without the id (as before the move) verifies",
                   record[names[1]]["identity"] == identities(version)[0])
            fake_http, run, state = world(identities(version)[1], repository_id=None)
            globals()["http"] = fake_http
            refused("the group's path without the project's id", lambda: verified_files(
                version, names, os.path.join(work, "c"), "cosign", run=run), "records no repository id")
            fake_http, run, state = world(identities(version)[0], repository_id="1267388815")
            globals()["http"] = fake_http
            refused("another project's id", lambda: verified_files(
                version, names, os.path.join(work, "d"), "cosign", run=run), "not 86730559")
            # Another project's pipeline at the group's path, this project's id planted in its
            # key in front of the extension naming its own: the extension is what counts.
            fake_http, run, state = world(identities(version)[1], repository_id="1267388815",
                                          key=b"\x00" * 7 + sigstore_certificate.planted())
            globals()["http"] = fake_http
            refused("another project's id, with this project's planted in the key", lambda: verified_files(
                version, names, os.path.join(work, "d2"), "cosign", run=run), "names the repository id 1267388815")
            fake_http, run, state = world(identities(version)[1], repository_id=None, key=sigstore_certificate.planted())
            globals()["http"] = fake_http
            refused("  ... and with no id of its own at the group's path", lambda: verified_files(
                version, names, os.path.join(work, "d3"), "cosign", run=run), "records no repository id")
            fake_http, run, state = world("https://gitlab.com/someone/else//.gitlab-ci.yml@refs/tags/v9.9.9")
            globals()["http"] = fake_http
            refused("a file signed by another pipeline", lambda: verified_files(
                version, names, os.path.join(work, "e"), "cosign", run=run), "does not verify as signed")
            fake_http, run, state = world(identities(version)[1], links={names[0]: "https://example.org/" + names[0]})
            globals()["http"] = fake_http
            refused("a link that is not the registry's", lambda: verified_files(
                version, names, os.path.join(work, "f"), "cosign", run=run), "has no link to")
            fake_http, run, state = world(identities(version)[1], missing=names[0] + BUNDLE)
            globals()["http"] = fake_http
            refused("a bundle the registry does not have", lambda: verified_files(
                version, names, os.path.join(work, "g"), "cosign", run=run), "answered 404")
        finally:
            globals()["http"] = held_http
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if cosign:
        cosign_cases(expect, refused, cosign)
    else:
        print("skip  the verification with a real cosign: no --cosign given")

    print("")
    if failures:
        print("%d self-test case(s) failed" % len(failures))
        return 1
    print("Every self-test case passed.")
    return 0


def cosign_cases(expect, refused, cosign):
    """verify_file with a real cosign, against build/ci/channels/fake-sigstore.py's
    bundles and trusted root."""
    fake_module = load("fake_sigstore", os.path.join("build", "ci", "channels", "fake-sigstore.py"))
    if fake_module.x509 is None:
        print("skip  the verification with a real cosign: the cryptography package is not installed")
        return
    fake = fake_module.FakeSigstore()
    version = "9.9.9"
    work = tempfile.mkdtemp(prefix="hm-channels-cosign-")
    try:
        root = fake.write_trusted_root(os.path.join(work, "trusted_root.json"))
        path = os.path.join(work, "hmailserver-9.9.9.tar.gz")
        content = b"a release file\n"
        with open(path, "wb") as f:
            f.write(content)

        def bundle(**kwargs):
            return fake.write_bundle(path + BUNDLE, content, **kwargs)

        bundle(identity=identities(version)[1], repository=GROUP_REPOSITORY, repository_id=PROJECT_ID)
        expect("cosign: the group's path with the project's id verifies",
               verify_file(path, path + BUNDLE, version, cosign, root) == (identities(version)[1], PROJECT_ID))
        bundle(identity=identities(version)[0], repository=REPOSITORIES[0], repository_id=PROJECT_ID)
        expect("cosign: the personal path verifies",
               verify_file(path, path + BUNDLE, version, cosign, root)[0] == identities(version)[0])
        bundle(identity=identities(version)[1], repository=GROUP_REPOSITORY)
        refused("cosign: the group's path without the id", lambda: verify_file(path, path + BUNDLE, version, cosign, root),
                "records no repository id")
        bundle(identity=identities(version)[0], repository_id="1267388815")
        refused("cosign: another project's id", lambda: verify_file(path, path + BUNDLE, version, cosign, root), "not 86730559")
        bundle(identity=identities("9.9.8")[1], repository_id=PROJECT_ID)
        refused("cosign: another tag's pipeline", lambda: verify_file(path, path + BUNDLE, version, cosign, root),
                "does not verify as signed")
        bundle(identity=identities(version)[1].replace("refs/tags/v", "refs/heads/batch"), repository_id=PROJECT_ID)
        refused("cosign: a branch's pipeline (a rehearsal)", lambda: verify_file(path, path + BUNDLE, version, cosign, root),
                "does not verify as signed")
        bundle(identity=identities(version)[1], repository_id=PROJECT_ID)
        with open(path, "ab") as f:
            f.write(b"tampered")
        refused("cosign: a file changed after it was signed", lambda: verify_file(path, path + BUNDLE, version, cosign, root),
                "does not verify as signed")
        refused("cosign: the public trust, which never issued the fixture's certificate",
                lambda: verify_file(path, path + BUNDLE, version, cosign, None), "does not verify as signed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--cosign", help="with --self-test: a cosign to prove the verification with")
    commands = parser.add_subparsers(dest="command")
    v = commands.add_parser("verify", help="the release's channel files, downloaded and verified")
    v.add_argument("--out", default="channels-verify")
    v.add_argument("names", nargs="*")
    t = commands.add_parser("fetch-tool", help="a tool's binary out of its pinned image")
    t.add_argument("tool", choices=sorted(TOOLS))
    t.add_argument("--into", required=True)
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test(arguments.cosign)
    try:
        if arguments.command == "verify":
            return verify_command(os.path.abspath(arguments.out), arguments.names)
        if arguments.command == "fetch-tool":
            fetch_tool(arguments.tool, arguments.into)
            return 0
    except Refused as error:
        say("REFUSED: %s" % error)
        return 1
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
