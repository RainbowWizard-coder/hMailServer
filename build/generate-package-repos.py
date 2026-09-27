#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The signed apt and dnf repositories on the update feed's site.

GitLab.com's package registry serves neither an apt nor a dnf repository: its
Debian registry is for self-managed instances only, and it has no RPM format.
The release's .deb and .rpm files are in the generic registry, where anyone can
download them, but nothing there signs repository metadata. This script makes
that metadata and adds it to the site GitLab Pages serves at
https://updates.progressiverobot.com, beside the update feed
(build/generate-update-feed.py), so that an administrator can install with
`apt install hmailserver` or `dnf install hmailserver` and take updates from
the package manager (docs/PackageManagers.md, "apt and dnf").

What it reads is the feed the pages job has just written: public/releases.json,
every release with each file's URL in the registry, its size and its SHA-256, as
the registry records them. Only stable releases are taken: a pre-release never
reaches an apt or dnf repository. What it adds under public/:

  apt/dists/<suite>/InRelease, Release, Release.gpg
  apt/dists/<suite>/main/binary-<arch>/Packages, Packages.gz
                          an apt repository, one suite per distribution release:
                          ubuntu26.04, ubuntu24.04, debian13, debian12 - the
                          value of $ID$VERSION_ID in that release's
                          /etc/os-release, so one sources line fits every one
  rpm/<dist>/<arch>/repodata/...     with repomd.xml.asc beside repomd.xml
  rpm/<dist>/hmailserver.repo        a dnf repository per dist tag: fc44, el9,
                          el10 and so on, the one in the .rpm's own name
  hmailserver-repository-key.asc     the public key that signs both
  _redirects              one rule per package file, appended

The packages themselves are not copied onto the site. apt's pool path and dnf's
Packages/ path each answer a 302 to the file in the package registry, by a rule
of its own in _redirects: Pages answers a file before a rule, and no file stands
there. Both clients follow the redirect (docs/PackageManagers.md says what was
run to show it). So the site stays at the size of its metadata, however many
releases the repositories keep, and each file is fetched from the same place the
release page and the update feed name, with the size and SHA-256 the signed
metadata gives for it.

Which file goes where comes from its name, as the package jobs make them:
  hmailserver_<v>-1~<distro>_<arch>.deb        suite <distro>
  hmailserver-<v>-<release>.<dist>.<arch>.rpm  repository <dist>
  hmailserver_<v>_<arch>.deb, the generic name, goes where GENERIC says, and
  only from the version it names: the generic files of older releases were
  built elsewhere and are left out. hmailserver-<v>-1.<arch>.rpm, the generic
  .rpm, goes in no repository (GENERIC says why).
A file of any other shape (openSUSE's .suse.lp160 and .suse.tw among them) is
not a package of these repositories and is passed over.

The signing key. The public key is committed at .gitlab/package-repository-key.asc
and published from there, so users trust what the repository says and not what
a job was handed. The secret is the signing subkey only, in the protected and
masked CI/CD variable HM_REPO_SIGNING_KEY (base64 of `gpg --export-secret-subkeys`,
or a File variable), with its passphrase, if it has one, in
HM_REPO_SIGNING_PASSPHRASE. While either the variable or the committed key is
missing, the script says so, adds nothing, and exits 0: the site is the update
feed alone, so a variable set before the key's commit reaches master never stops
the feed's deployment. A key that is there but does not sign as the committed key is a
failure, and nothing is deployed.

Usage, from the repository root:
  python3 build/generate-package-repos.py --site public [--cache DIR] [--keep N]
      add the repositories to the site the pages job wrote
  python3 build/generate-package-repos.py --check public
      read the repositories as apt and dnf would: every signature by the
      committed key, every hash, every package's redirect to a registry file of
      the size and SHA-256 releases.json gives
  python3 build/generate-package-repos.py --serve DIR [--port N] [--bind ADDR]
      serve DIR as GitLab Pages would, _redirects included, for a client to try
  python3 build/generate-package-repos.py --self-test
      the naming, selection, redirect and refusal rules against canned
      releases, and, when gpg, dpkg-deb and apt-ftparchive are there, a whole
      apt repository built, checked and tampered with; no network

Tools: apt-ftparchive (apt-utils), dpkg-deb (dpkg), createrepo_c and gpg, from
the distribution's own packages; the CI job takes them from Debian 13.
"""
import argparse
import base64
import gzip
import hashlib
import http.server
import io
import json
import os
import re
import shutil
import socketserver
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ElementTree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PROJECT_ID = "86730559"
REGISTRY = "https://gitlab.com/api/v4/projects/" + PROJECT_ID + "/packages/generic/"
SITE = "https://updates.progressiverobot.com"
PUBLIC_KEY = os.path.join(ROOT, ".gitlab", "package-repository-key.asc")
SITE_KEY = "hmailserver-repository-key.asc"
KEY_VARIABLE = "HM_REPO_SIGNING_KEY"
PASSPHRASE_VARIABLE = "HM_REPO_SIGNING_PASSPHRASE"

# Where a generic-named file goes, and the first version whose generic files were
# built there. 6.3.4's .deb was built on linix in Ubuntu 26.04, and linux-package-deb
# installs it there with apt and its dependencies; from 6.3.5 the package jobs keep
# the generic names for the Ubuntu 26.04 build (docs/PackagesDeb.md). The generic
# .debs of 6.3.0 to 6.3.3 came from GitHub's Ubuntu 24.04 runners and are left out
# rather than offered to a release they may not install on.
#
# The generic .rpm goes nowhere. linux-build makes it on Ubuntu 26.04, so its
# automatic Requires are that build's sonames and symbol versions, and
# linux-package-rpm installs it on Fedora with --nodeps
# (build/ci/linux-package-install.sh). dnf resolved master's build of 26 September
# 2026 on Fedora 43 and 44 and AlmaLinux 10 and refused it on AlmaLinux 9, but it is
# no RPM distribution's own build. Where the Fedora 44 build is copied under the
# generic name, the .fc44 file beside it is the same build. So a dnf repository
# holds the per-distribution builds alone, from the first release that carries them.
GENERIC = {"deb": ("ubuntu26.04", "6.3.4")}

DEB_NAME = re.compile(r"^hmailserver_(?P<version>[0-9][0-9.]*[0-9])(?:-[0-9]+~(?P<distro>[a-z]+[0-9][0-9.]*))?_(?P<arch>amd64|arm64)\.deb$")
RPM_NAME = re.compile(r"^hmailserver-(?P<version>[0-9][0-9.]*[0-9])-[0-9]+(?:\.(?P<dist>[a-z]+[0-9]+))?\.(?P<arch>x86_64|aarch64)\.rpm$")
STABLE_TAG = re.compile(r"^v([0-9]+(?:\.[0-9]+)*)$")

APT_POOL = "apt/pool/main/h/hmailserver/"
# gitlab-pages' limits on _redirects (internal/redirects/redirects.go): rules past
# the 1,000th are not read, and a file over 64 KiB is not read at all.
REDIRECTS_MAX_BYTES = 64 * 1024
REDIRECTS_MAX_RULES = 1000


class RepoError(Exception):
    pass


def say(text=""):
    print(text, flush=True)


# ---------------------------------------------------------------- what goes where

def version_key(text):
    return tuple(int(part) for part in text.split("."))


def classify(name, version):
    """(kind, where, arch) for a package file of the release `version`, or None
    when the file is not one of these repositories' packages, with the reason."""
    match = DEB_NAME.match(name)
    if match:
        kind, where, arch = "deb", match.group("distro"), match.group("arch")
    else:
        match = RPM_NAME.match(name)
        if not match:
            return None, None
        kind, where, arch = "rpm", match.group("dist"), match.group("arch")
    if match.group("version") != version:
        return None, "%s names the version %s, not the release's %s" % (name, match.group("version"), version)
    generic = where is None
    if generic:
        if kind not in GENERIC:
            return None, "%s has the generic name, built on Ubuntu, which goes in no %s repository" % (name, kind)
        where, since = GENERIC[kind]
        if version_key(version) < version_key(since):
            return None, "%s has the generic name, which goes to %s only from %s" % (name, where, since)
    return (kind, where, arch, generic), None


def select(releases, registry=REGISTRY, keep=0):
    """The package files of the stable releases in releases.json, newest version
    first, and a warning for each file passed over for a reason worth saying."""
    stable = []
    warnings = []
    for release in releases:
        tag = release.get("tag_name") or ""
        match = STABLE_TAG.match(tag)
        if release.get("prerelease") or release.get("draft") or not match:
            continue
        stable.append((version_key(match.group(1)), match.group(1), release))
    stable.sort(key=lambda item: item[0], reverse=True)
    if keep:
        stable = stable[:keep]

    entries = []
    for _, version, release in stable:
        for asset in release.get("assets") or []:
            name = asset.get("name") or ""
            placed, why = classify(name, version)
            if placed is None:
                if why:
                    warnings.append(why)
                continue
            url = asset.get("browser_download_url") or ""
            expected = registry + "hmailserver/" + version + "/"
            if not url.startswith(expected) or urllib.parse.unquote(url[len(expected):]) != name:
                warnings.append("%s is left out: its URL %s is not the file %s%s" % (name, url, expected, name))
                continue
            digest = asset.get("digest") or ""
            if not re.match(r"^sha256:[0-9a-f]{64}$", digest):
                warnings.append("%s is left out: releases.json gives no SHA-256 for it" % name)
                continue
            kind, where, arch, generic = placed
            entries.append({"kind": kind, "where": where, "arch": arch, "name": name, "version": version, "generic": generic,
                            "url": url, "sha256": digest[len("sha256:"):], "size": int(asset.get("size") or 0)})
    # Were a release ever to carry both a generic file and a suffixed one for the
    # same place, the suffixed one says where it belongs, and the repository takes
    # that one. (The generic .rpm beside -1.fc44.<arch>.rpm never gets this far.)
    suffixed = set((e["version"], e["kind"], e["where"], e["arch"]) for e in entries if not e["generic"])
    kept = []
    for entry in entries:
        if entry["generic"] and (entry["version"], entry["kind"], entry["where"], entry["arch"]) in suffixed:
            warnings.append("%s is left out: the release has the %s build under its own name" % (entry["name"], entry["where"]))
            continue
        kept.append(entry)
    return kept, warnings


def site_path(entry):
    """Where the client asks for the package on the site: the path a rule sends
    to the registry."""
    if entry["kind"] == "deb":
        return "/" + APT_POOL + entry["name"]
    return "/rpm/%s/%s/Packages/%s" % (entry["where"], entry["arch"], entry["name"])


# ---------------------------------------------------------------- _redirects

def parse_rules(text):
    rules = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        rules.append((parts[0], parts[1] if len(parts) > 1 else "", parts[2] if len(parts) > 2 else "301"))
    return rules


def add_redirects(site, entries):
    """Append a 302 rule per package to site/_redirects, which the feed may have
    written already, and hold the whole file to what Pages reads."""
    path = os.path.join(site, "_redirects")
    existing = ""
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            existing = f.read()
    taken = set(source for source, _, _ in parse_rules(existing))
    lines = []
    for entry in sorted(entries, key=site_path):
        source = site_path(entry)
        if source in taken:
            raise RepoError("_redirects already has a rule for %s" % source)
        if os.path.exists(os.path.join(site, *source.strip("/").split("/"))):
            raise RepoError("a file stands at %s, which Pages would answer before the rule" % source)
        taken.add(source)
        lines.append("%s %s 302" % (source, entry["url"]))
    text = existing
    if text and not text.endswith("\n"):
        text += "\n"
    if lines:
        text += "# The apt and dnf repositories' packages, in the package registry (build/generate-package-repos.py).\n"
        text += "\n".join(lines) + "\n"
    data = text.encode("utf-8")
    count = len(parse_rules(text))
    if len(data) > REDIRECTS_MAX_BYTES:
        raise RepoError("_redirects would be %d bytes; Pages reads none of a file over %d. Keep fewer releases (--keep)." % (
            len(data), REDIRECTS_MAX_BYTES))
    if count > REDIRECTS_MAX_RULES:
        raise RepoError("_redirects would hold %d rules; Pages reads the first %d. Keep fewer releases (--keep)." % (
            count, REDIRECTS_MAX_RULES))
    with open(path, "wb") as f:
        f.write(data)
    return len(lines), count, len(data)


# ---------------------------------------------------------------- the files

def fetch(entry, cache, attempts=5):
    """The package file, from the cache or the registry, held to the size and
    SHA-256 releases.json gives. The cache is keyed by that hash."""
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, entry["sha256"])
    if os.path.isfile(path) and sha256_of(path) == entry["sha256"]:
        return path
    partial = path + ".part"
    for attempt in range(attempts):
        try:
            request = urllib.request.Request(entry["url"], headers={"User-Agent": "hmailserver-package-repos"})
            with urllib.request.urlopen(request, timeout=120) as response, open(partial, "wb") as f:
                shutil.copyfileobj(response, f, 1 << 20)
            break
        except (urllib.error.URLError, OSError) as e:
            if attempt == attempts - 1:
                raise RepoError("%s could not be downloaded: %s" % (entry["url"], e))
            time.sleep(5 * (attempt + 1))
    size = os.path.getsize(partial)
    digest = sha256_of(partial)
    if digest != entry["sha256"] or (entry["size"] and size != entry["size"]):
        os.remove(partial)
        raise RepoError("%s is %d bytes with SHA-256 %s; releases.json says %d bytes and %s" % (
            entry["url"], size, digest, entry["size"], entry["sha256"]))
    os.replace(partial, path)
    return path


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def place(source, target):
    os.makedirs(os.path.dirname(target), exist_ok=True)
    try:
        os.link(source, target)
    except OSError:
        shutil.copyfile(source, target)


def run(command, cwd=None, stdin=None):
    result = subprocess.run(command, cwd=cwd, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RepoError("%s failed (exit %d): %s" % (" ".join(command[:3]), result.returncode,
                                                     result.stderr.decode("utf-8", "replace").strip()[-2000:]))
    return result.stdout


def need_tools(names):
    missing = [name for name in names if shutil.which(name) is None]
    if missing:
        raise RepoError("%s not found; the job installs them from Debian's apt-utils, createrepo-c and gnupg" % ", ".join(missing))


# ---------------------------------------------------------------- signing

def read_secret(value):
    """The secret key's bytes from the variable's value: a File variable's path,
    ASCII armour, or base64 of either form."""
    value = value.strip()
    if os.path.isfile(value):
        with open(value, "rb") as f:
            value = f.read().decode("utf-8", "replace").strip()
    if value.startswith("-----BEGIN PGP PRIVATE KEY BLOCK-----"):
        return value.encode("utf-8")
    try:
        data = base64.b64decode("".join(value.split()), validate=True)
    except ValueError:
        raise RepoError("%s is neither ASCII armour nor base64" % KEY_VARIABLE)
    if not data:
        raise RepoError("%s is empty once decoded" % KEY_VARIABLE)
    return data


def colons(output):
    return [line.split(":") for line in output.decode("utf-8", "replace").splitlines()]


def key_fingerprint(armoured_path):
    """The primary fingerprint of the one public key in a file."""
    home = tempfile.mkdtemp(prefix="hm-gpg-")
    try:
        out = run(["gpg", "--homedir", home, "--batch", "--with-colons", "--import-options", "show-only",
                   "--import", armoured_path])
        fingerprints = []
        primary = False
        for fields in colons(out):
            if fields[0] == "pub":
                primary = True
            elif fields[0] == "fpr" and primary:
                fingerprints.append(fields[9])
                primary = False
        if len(fingerprints) != 1:
            raise RepoError("%s holds %d keys, not one" % (armoured_path, len(fingerprints)))
        return fingerprints[0]
    finally:
        shutil.rmtree(home, ignore_errors=True)


class Signer:
    """A throwaway GnuPG home with the committed public key and the secret from
    the variable, refusing a secret that is not a signing key of that public key."""

    def __init__(self, public_key, secret, passphrase):
        self.public_key = public_key
        self.secret = secret
        self.passphrase = passphrase
        self.home = None

    def __enter__(self):
        self.home = tempfile.mkdtemp(prefix="hm-gpg-")
        # Owner-only: GnuPG refuses a home others can read. The rule takes the
        # directory's search bit for an executable file's.
        os.chmod(self.home, 0o700)  # nosemgrep
        try:
            self.fingerprint = key_fingerprint(self.public_key)
            self.gpg(["--import", self.public_key])
            self.gpg(["--import"], stdin=self.secret)
            out = self.gpg(["--with-colons", "--list-secret-keys", self.fingerprint], passphrase=False, check=False)
            usable = []
            whole = False
            for fields in colons(out or b""):
                if fields[0] in ("sec", "ssb") and len(fields) > 14:
                    expired = fields[1] in ("e", "r")
                    if "s" in fields[11] and fields[14] != "#" and not expired:
                        usable.append(fields[4])
                    if fields[0] == "sec" and fields[14] != "#":
                        whole = True
            if not usable:
                raise RepoError("%s holds no usable signing key of %s, the committed %s: a different key, an expired or "
                                "revoked one, or a key without a signing subkey" % (
                                    KEY_VARIABLE, self.fingerprint, os.path.relpath(self.public_key, ROOT)))
            if whole:
                say("NOTE: %s holds the primary key's secret too; export the signing subkey alone "
                    "(docs/PackageManagers.md, \"The signing key\")." % KEY_VARIABLE)
            # A signature made now must verify with the committed key alone.
            probe = os.path.join(self.home, "probe")
            with open(probe, "wb") as f:
                f.write(b"hMailServer package repository signing probe\n")
            self.sign(probe, probe + ".asc", clear=False)
            verify(probe, probe + ".asc", self.public_key, self.fingerprint)
            return self
        except BaseException:
            shutil.rmtree(self.home, ignore_errors=True)
            raise

    def __exit__(self, *exc):
        shutil.rmtree(self.home, ignore_errors=True)

    def gpg(self, arguments, stdin=None, passphrase=False, check=True):
        command = ["gpg", "--homedir", self.home, "--batch", "--yes", "--no-tty"]
        if passphrase:
            command += ["--pinentry-mode", "loopback", "--passphrase-fd", "0"]
            stdin = (self.passphrase or "").encode("utf-8") + b"\n"
        command += arguments
        if check:
            return run(command, stdin=stdin)
        result = subprocess.run(command, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return result.stdout if result.returncode == 0 else None

    def sign(self, path, output, clear):
        mode = ["--clearsign"] if clear else ["--armor", "--detach-sign"]
        self.gpg(["--local-user", self.fingerprint, "--digest-algo", "SHA512"] + mode + ["--output", output, path],
                 passphrase=True)


def verify(path, signature, public_key, fingerprint):
    """Check one signature against the public key alone, as a client would: the
    VALIDSIG must be by `fingerprint` or a subkey of it. For a clear-signed file,
    pass it as `signature` and None as `path`."""
    home = tempfile.mkdtemp(prefix="hm-gpg-")
    try:
        run(["gpg", "--homedir", home, "--batch", "--import", public_key])
        command = ["gpg", "--homedir", home, "--batch", "--status-fd", "1", "--verify", signature]
        if path:
            command.append(path)
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        valid = [line.split() for line in result.stdout.decode("utf-8", "replace").splitlines()
                 if line.startswith("[GNUPG:] VALIDSIG ")]
        if result.returncode != 0 or not valid or valid[0][-1] != fingerprint:
            raise RepoError("%s does not verify with %s: %s" % (
                os.path.basename(signature), fingerprint, result.stderr.decode("utf-8", "replace").strip()[-500:]))
    finally:
        shutil.rmtree(home, ignore_errors=True)


def expiry_note(public_key, fingerprint):
    """A warning when the signing subkey expires within 180 days."""
    home = tempfile.mkdtemp(prefix="hm-gpg-")
    try:
        out = run(["gpg", "--homedir", home, "--batch", "--with-colons", "--import-options", "show-only", "--import", public_key])
        soonest = None
        for fields in colons(out):
            if fields[0] in ("pub", "sub") and "s" in fields[11] and fields[6]:
                expires = int(fields[6])
                soonest = expires if soonest is None else min(soonest, expires)
        if soonest and soonest - time.time() < 180 * 86400:
            return "the signing key expires on %s: extend it and commit the key again (docs/PackageManagers.md)" % (
                time.strftime("%Y-%m-%d", time.gmtime(soonest)))
        return None
    finally:
        shutil.rmtree(home, ignore_errors=True)


# ---------------------------------------------------------------- the repositories

def build_apt(entries, files, work, site, signer):
    made = []
    if any(e["kind"] == "deb" for e in entries):
        need_tools(["apt-ftparchive"])
    for suite in sorted(set(e["where"] for e in entries if e["kind"] == "deb")):
        mine = [e for e in entries if e["kind"] == "deb" and e["where"] == suite]
        stage = os.path.join(work, "apt", suite)
        for entry in mine:
            place(files[entry["name"]], os.path.join(stage, APT_POOL[len("apt/"):], entry["name"]))
        arches = sorted(set(e["arch"] for e in mine))
        dists = os.path.join(site, "apt", "dists", suite)
        for arch in arches:
            packages = run(["apt-ftparchive", "--arch", arch, "-o", "APT::FTPArchive::MD5=false",
                            "-o", "APT::FTPArchive::SHA1=false", "packages", "pool"], cwd=stage)
            target = os.path.join(dists, "main", "binary-" + arch)
            os.makedirs(target, exist_ok=True)
            with open(os.path.join(target, "Packages"), "wb") as f:
                f.write(packages)
            with open(os.path.join(target, "Packages.gz"), "wb") as f:
                f.write(gzip.compress(packages, 9, mtime=0))
            with open(os.path.join(target, "Release"), "w", encoding="utf-8") as f:
                f.write("Archive: %s\nComponent: main\nOrigin: hMailServer\nLabel: hMailServer\nArchitecture: %s\n" % (suite, arch))
        options = []
        for field, value in (("Origin", "hMailServer"), ("Label", "hMailServer"), ("Suite", suite), ("Codename", suite),
                             ("Architectures", " ".join(arches)), ("Components", "main"),
                             ("Description", "hMailServer for %s, from %s" % (suite, SITE))):
            options += ["-o", "APT::FTPArchive::Release::%s=%s" % (field, value)]
        release = run(["apt-ftparchive", "-o", "APT::FTPArchive::MD5=false", "-o", "APT::FTPArchive::SHA1=false"]
                      + options + ["release", "."], cwd=dists)
        with open(os.path.join(dists, "Release"), "wb") as f:
            f.write(release)
        signer.sign(os.path.join(dists, "Release"), os.path.join(dists, "InRelease"), clear=True)
        signer.sign(os.path.join(dists, "Release"), os.path.join(dists, "Release.gpg"), clear=False)
        made.append("apt %s: %s, %d package(s)" % (suite, " ".join(arches), len(mine)))
    return made


REPO_FILE = """[hmailserver]
name=hMailServer ({dist}, $basearch)
baseurl={site}/rpm/{dist}/$basearch/
enabled=1
# The metadata is signed (repomd.xml.asc) and names each package's SHA-256. The
# packages themselves are the release's files, which carry no RPM signature.
repo_gpgcheck=1
gpgcheck=0
gpgkey={site}/{key}
"""


def build_rpm(entries, files, work, site, signer):
    made = []
    if any(e["kind"] == "rpm" for e in entries):
        need_tools(["createrepo_c"])
    for dist in sorted(set(e["where"] for e in entries if e["kind"] == "rpm")):
        arches = sorted(set(e["arch"] for e in entries if e["kind"] == "rpm" and e["where"] == dist))
        for arch in arches:
            mine = [e for e in entries if e["kind"] == "rpm" and e["where"] == dist and e["arch"] == arch]
            stage = os.path.join(work, "rpm", dist, arch)
            for entry in mine:
                place(files[entry["name"]], os.path.join(stage, "Packages", entry["name"]))
            run(["createrepo_c", "--quiet", "--no-database", "--general-compress-type", "gz", "--checksum", "sha256", stage])
            target = os.path.join(site, "rpm", dist, arch, "repodata")
            shutil.copytree(os.path.join(stage, "repodata"), target)
            signer.sign(os.path.join(target, "repomd.xml"), os.path.join(target, "repomd.xml.asc"), clear=False)
            made.append("rpm %s/%s: %d package(s)" % (dist, arch, len(mine)))
        with open(os.path.join(site, "rpm", dist, "hmailserver.repo"), "w", encoding="utf-8", newline="\n") as f:
            f.write(REPO_FILE.format(dist=dist, site=SITE, key=SITE_KEY))
    return made


def publish(site, cache, keep, public_key=PUBLIC_KEY, registry=REGISTRY, environ=os.environ):
    """Add the repositories to the site. 0 when they were added or refused for a
    missing key (the site is then the feed alone), 1 on a failure."""
    releases_path = os.path.join(site, "releases.json")
    if not os.path.isfile(releases_path):
        raise RepoError("%s has no releases.json: run build/generate-update-feed.py --output %s first" % (site, site))
    secret_value = (environ.get(KEY_VARIABLE) or "").strip()
    missing = []
    if not secret_value:
        missing.append("the CI/CD variable %s is not set (protected and masked: it reaches only a protected branch "
                       "or tag)" % KEY_VARIABLE)
    if not os.path.isfile(public_key):
        missing.append("%s, the public key users trust, is not committed" % os.path.relpath(public_key, ROOT))
    if missing:
        say("REFUSED: no apt or dnf repository is published, so the site is the update feed alone:")
        for line in missing:
            say("  - " + line)
        say("The owner makes the key and sets both (docs/PackageManagers.md, \"The signing key\").")
        if secret_value:
            say("The variable is set, but there is no committed public key to check it against, so it is not used.")
        return 0

    with open(releases_path, encoding="utf-8") as f:
        releases = json.load(f)
    entries, warnings = select(releases, registry, keep)
    for warning in warnings:
        say("passed over: " + warning)
    if not entries:
        raise RepoError("releases.json names no .deb or .rpm a repository takes")
    for existing in ("apt", "rpm", SITE_KEY):
        if os.path.exists(os.path.join(site, existing)):
            raise RepoError("%s already has %s: this runs once, on the feed the pages job wrote" % (site, existing))

    files = {}
    for entry in entries:
        files[entry["name"]] = fetch(entry, cache)
        say("%-60s %9d  sha256 %s" % (entry["name"], entry["size"], entry["sha256"][:16]))

    passphrase = environ.get(PASSPHRASE_VARIABLE) or ""
    work = tempfile.mkdtemp(prefix="hm-repos-")
    try:
        with Signer(public_key, read_secret(secret_value), passphrase) as signer:
            say("signing as %s" % signer.fingerprint)
            made = build_apt(entries, files, work, site, signer) + build_rpm(entries, files, work, site, signer)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    shutil.copyfile(public_key, os.path.join(site, SITE_KEY))
    added, count, size = add_redirects(site, entries)
    for line in made:
        say(line)
    say("_redirects: %d rules added, %d in all, %d bytes (Pages reads %d rules and %d bytes)." % (
        added, count, size, REDIRECTS_MAX_RULES, REDIRECTS_MAX_BYTES))
    note = expiry_note(public_key, key_fingerprint(public_key))
    if note:
        say("WARNING: " + note)
    return 0


# ---------------------------------------------------------------- reading it back

def release_files(text):
    """{path: (size, sha256)} from a Release file's SHA256 section."""
    listed = {}
    section = None
    for line in text.splitlines():
        if not line.startswith(" "):
            section = line.split(":", 1)[0]
            continue
        if section == "SHA256":
            digest, size, path = line.split()
            listed[path] = (int(size), digest)
    return listed


def stanzas(text):
    for block in text.strip().split("\n\n"):
        fields = {}
        for line in block.splitlines():
            if line[:1] not in (" ", "\t") and ":" in line:
                key, value = line.split(":", 1)
                fields[key] = value.strip()
        if fields:
            yield fields


def check(site, public_key=None, registry=REGISTRY):
    """Problems a client would meet, read from the files as apt and dnf read them."""
    problems = []
    key = public_key or os.path.join(site, SITE_KEY)
    if not os.path.isfile(key):
        return ["%s is missing, so nothing can be verified" % key]
    fingerprint = key_fingerprint(key)
    with open(os.path.join(site, "releases.json"), encoding="utf-8") as f:
        expected = dict((e["name"], e) for e in select(json.load(f), registry)[0])
    rules = {}
    if os.path.isfile(os.path.join(site, "_redirects")):
        with open(os.path.join(site, "_redirects"), encoding="utf-8") as f:
            rules = dict((source, (target, status)) for source, target, status in parse_rules(f.read()))

    def served(path, name, size, sha256):
        entry = expected.get(name)
        rule = rules.get(path)
        if entry is None:
            problems.append("%s is in the metadata but is no stable release's file in releases.json" % name)
        elif (size, sha256) != (entry["size"], entry["sha256"]):
            problems.append("%s: the metadata says %d bytes, %s; releases.json says %d, %s" % (
                name, size, sha256, entry["size"], entry["sha256"]))
        if rule is None:
            problems.append("no _redirects rule for %s" % path)
        elif entry is not None and rule != (entry["url"], "302"):
            problems.append("%s goes to %s %s, not 302 to %s" % (path, rule[1], rule[0], entry["url"]))
        seen.add(name)

    seen = set()
    dists = os.path.join(site, "apt", "dists")
    for suite in sorted(os.listdir(dists)) if os.path.isdir(dists) else []:
        base = os.path.join(dists, suite)
        try:
            verify(None, os.path.join(base, "InRelease"), key, fingerprint)
            verify(os.path.join(base, "Release"), os.path.join(base, "Release.gpg"), key, fingerprint)
        except RepoError as e:
            problems.append("apt %s: %s" % (suite, e))
            continue
        with open(os.path.join(base, "InRelease"), encoding="utf-8") as f:
            signed = f.read()
        with open(os.path.join(base, "Release"), encoding="utf-8") as f:
            release = f.read()
        if release not in signed:
            problems.append("apt %s: InRelease and Release say different things" % suite)
        listed = release_files(release)
        for path, (size, digest) in sorted(listed.items()):
            full = os.path.join(base, *path.split("/"))
            if not os.path.isfile(full) or os.path.getsize(full) != size or sha256_of(full) != digest:
                problems.append("apt %s: %s is not what Release says" % (suite, path))
        for path in sorted(listed):
            if not path.endswith("/Packages"):
                continue
            with open(os.path.join(base, *path.split("/")), encoding="utf-8") as f:
                for fields in stanzas(f.read()):
                    filename = fields.get("Filename", "")
                    served("/apt/" + filename, filename.rsplit("/", 1)[-1], int(fields.get("Size", "0")), fields.get("SHA256", ""))
    rpm = os.path.join(site, "rpm")
    for dist in sorted(os.listdir(rpm)) if os.path.isdir(rpm) else []:
        for arch in sorted(os.listdir(os.path.join(rpm, dist))):
            repodata = os.path.join(rpm, dist, arch, "repodata")
            if not os.path.isdir(repodata):
                continue
            try:
                verify(os.path.join(repodata, "repomd.xml"), os.path.join(repodata, "repomd.xml.asc"), key, fingerprint)
            except RepoError as e:
                problems.append("rpm %s/%s: %s" % (dist, arch, e))
                continue
            ns = {"r": "http://linux.duke.edu/metadata/repo", "c": "http://linux.duke.edu/metadata/common"}
            # createrepo_c's own output, written by this run a moment ago; and the
            # standard library's parser resolves no external entity (Python 3.7.1
            # on) and is not open to entity expansion with the Expat every
            # supported Python carries (build/ci/security-findings.py says the same).
            repomd = ElementTree.parse(os.path.join(repodata, "repomd.xml"))  # nosemgrep
            for data in repomd.getroot().findall("r:data", ns):
                href = data.find("r:location", ns).get("href")
                digest = data.find("r:checksum", ns).text
                full = os.path.join(rpm, dist, arch, *href.split("/"))
                if not os.path.isfile(full) or sha256_of(full) != digest:
                    problems.append("rpm %s/%s: %s is not what repomd.xml says" % (dist, arch, href))
                    continue
                if data.get("type") != "primary":
                    continue
                with gzip.open(full) as f:
                    primary = ElementTree.parse(f)  # nosemgrep
                for package in primary.getroot().findall("c:package", ns):
                    href = package.find("c:location", ns).get("href")
                    name = href.rsplit("/", 1)[-1]
                    served("/rpm/%s/%s/%s" % (dist, arch, href), name, int(package.find("c:size", ns).get("package")),
                           package.find("c:checksum", ns).text)
        if not os.path.isfile(os.path.join(rpm, dist, "hmailserver.repo")):
            problems.append("rpm %s has no hmailserver.repo" % dist)
    for name in sorted(set(expected) - seen):
        problems.append("%s, a stable release's package, is in no repository" % name)
    return problems


# ---------------------------------------------------------------- serving it

def pages_answer(root, raw_path):
    """(status, file path or Location) as GitLab Pages answers a GET: a file
    first, a directory's index.html, then the first _redirects rule whose from
    is the path. Pages matches the decoded path and ignores a trailing slash."""
    path = urllib.parse.unquote(raw_path.split("?", 1)[0])
    parts = [p for p in path.split("/") if p]
    if any(p in (".", "..") for p in parts):
        return 404, None
    full = os.path.join(root, *parts)
    if os.path.isfile(full):
        return 200, full
    if os.path.isdir(full) and os.path.isfile(os.path.join(full, "index.html")):
        return 200, os.path.join(full, "index.html")
    redirects = os.path.join(root, "_redirects")
    if os.path.isfile(redirects):
        with open(redirects, encoding="utf-8") as f:
            for source, target, status in parse_rules(f.read())[:REDIRECTS_MAX_RULES]:
                if source.rstrip("/") == path.rstrip("/") and status in ("301", "302"):
                    return int(status), target
    return 404, None


def serve(root, bind, port):
    class Handler(http.server.BaseHTTPRequestHandler):
        def answer(self, body):
            status, where = pages_answer(root, self.path)
            self.send_response(status)
            if status in (301, 302):
                self.send_header("Location", where)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if status != 200:
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_header("Content-Length", str(os.path.getsize(where)))
            self.end_headers()
            if body:
                with open(where, "rb") as f:
                    shutil.copyfileobj(f, self.wfile)

        def do_GET(self):
            self.answer(True)

        def do_HEAD(self):
            self.answer(False)

    socketserver.ThreadingTCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer((bind, port), Handler) as server:
        say("serving %s as Pages would on http://%s:%d/" % (root, bind, port))
        server.serve_forever()


# ---------------------------------------------------------------- self-test

def self_test():
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    def asset(version, name, digest=None):
        return {"name": name, "size": 1000, "digest": "sha256:" + (digest or hashlib.sha256(name.encode()).hexdigest()),
                "browser_download_url": REGISTRY + "hmailserver/%s/%s" % (version, urllib.parse.quote(name))}

    def release(tag, names, prerelease=False):
        return {"tag_name": tag, "prerelease": prerelease, "draft": False,
                "assets": [asset(tag[1:], n) for n in names]}

    releases = [
        release("v6.4.0-rc1", ["hmailserver_6.4.0-rc1_amd64.deb", "hmailserver-6.4.0-1.x86_64.rpm"], prerelease=True),
        release("v6.3.5", ["hMailServer-6.3.5-x64.exe", "hmailserver_6.3.5_amd64.deb", "hmailserver_6.3.5_arm64.deb",
                           "hmailserver_6.3.5-1~debian12_amd64.deb", "hmailserver_6.3.5-1~ubuntu24.04_arm64.deb",
                           "hmailserver_6.3.5_amd64.deb.cosign.bundle", "hmailserver-6.3.5-1.x86_64.rpm",
                           "hmailserver-6.3.5-1.fc44.aarch64.rpm", "hmailserver-6.3.5-1.el9.x86_64.rpm",
                           "hmailserver-6.3.5-1.aarch64.rpm",
                           "hmailserver_6.3.4_amd64.deb", "hMailServer-6.3.5-x86_64.AppImage"]),
        release("v6.3.4", ["hmailserver_6.3.4_amd64.deb", "hmailserver-6.3.4-1.x86_64.rpm"]),
        release("v6.3.3", ["hmailserver_6.3.3_amd64.deb", "hmailserver-6.3.3-1.x86_64.rpm"]),
    ]
    entries, warnings = select(releases)
    placed = sorted((e["name"], e["kind"], e["where"], e["arch"]) for e in entries)
    expect(placed == sorted([
        ("hmailserver_6.3.5_amd64.deb", "deb", "ubuntu26.04", "amd64"),
        ("hmailserver_6.3.5_arm64.deb", "deb", "ubuntu26.04", "arm64"),
        ("hmailserver_6.3.5-1~debian12_amd64.deb", "deb", "debian12", "amd64"),
        ("hmailserver_6.3.5-1~ubuntu24.04_arm64.deb", "deb", "ubuntu24.04", "arm64"),
        ("hmailserver-6.3.5-1.fc44.aarch64.rpm", "rpm", "fc44", "aarch64"),
        ("hmailserver-6.3.5-1.el9.x86_64.rpm", "rpm", "el9", "x86_64"),
        ("hmailserver_6.3.4_amd64.deb", "deb", "ubuntu26.04", "amd64"),
    ]), "the files are placed by name: %s" % placed)
    expect(not any("6.4.0" in e["name"] for e in entries), "a pre-release's files are left out")
    expect(any("6.3.3_amd64.deb has the generic name" in w for w in warnings), "6.3.3's generic .deb is passed over, and said")
    expect(any("hmailserver_6.3.4_amd64.deb names the version 6.3.4, not the release's 6.3.5" in w for w in warnings),
           "a file of another version in a release is passed over, and said")
    for name in ("hmailserver-6.3.5-1.x86_64.rpm", "hmailserver-6.3.5-1.aarch64.rpm", "hmailserver-6.3.4-1.x86_64.rpm"):
        expect(any(name + " has the generic name, built on Ubuntu, which goes in no rpm repository" in w for w in warnings),
               "the generic .rpm %s goes in no repository, and said" % name)
    both = select([release("v6.3.5", ["hmailserver_6.3.5_amd64.deb", "hmailserver_6.3.5-1~ubuntu26.04_amd64.deb"])])
    expect([e["name"] for e in both[0]] == ["hmailserver_6.3.5-1~ubuntu26.04_amd64.deb"]
           and any("hmailserver_6.3.5_amd64.deb is left out: the release has the ubuntu26.04 build" in w for w in both[1]),
           "a generic file is left out where the release has the suffixed build for the same place")
    expect(not any("AppImage" in w or "exe" in w or "bundle" in w for w in warnings), "files of other kinds pass silently")
    expect([e["version"] for e in select(releases, keep=1)[0]] == ["6.3.5"] * 6, "--keep 1 keeps the newest version only")

    foreign = [release("v6.3.5", ["hmailserver_6.3.5_amd64.deb"])]
    foreign[0]["assets"][0]["browser_download_url"] = "https://example.com/hmailserver_6.3.5_amd64.deb"
    other = [release("v6.3.5", ["hmailserver_6.3.5_amd64.deb"])]
    other[0]["assets"][0]["browser_download_url"] = REGISTRY + "hmailserver/6.3.4/hmailserver_6.3.5_amd64.deb"
    nodigest = [release("v6.3.5", ["hmailserver_6.3.5_amd64.deb"])]
    nodigest[0]["assets"][0]["digest"] = ""
    for name, case in (("a URL outside the registry", foreign), ("another version's package", other), ("no SHA-256", nodigest)):
        got, why = select(case)
        expect(not got and why, "a file with %s is left out, and said" % name)
    expect(classify("hmailserver-6.3.4-1.x86_64.rpm", "6.3.4")[0] is None, "the generic .rpm of 6.3.4 goes nowhere")
    expect(classify("hmailserver_6.3.4_amd64.deb", "6.3.4")[0] == ("deb", "ubuntu26.04", "amd64", True), "the generic .deb of 6.3.4")
    for bad in ("hmailserver_6.3.5-1~../x_amd64.deb", "hmailserver_6.3.5_i386.deb", "hmailserver-6.3.5-1.fc44.src.rpm",
                "hmailserver-6.3.5-1.fc 44.x86_64.rpm", "hmailserver-dbg_6.3.5_amd64.deb"):
        expect(classify(bad, "6.3.5") == (None, None), "%s is not a package of these repositories" % bad)

    paths = sorted(site_path(e) for e in entries)
    expect("/apt/pool/main/h/hmailserver/hmailserver_6.3.5-1~debian12_amd64.deb" in paths
           and "/rpm/el9/x86_64/Packages/hmailserver-6.3.5-1.el9.x86_64.rpm" in paths, "the paths the clients ask for")
    expect(all(p.count("/") <= 25 for p in paths), "no path has more slashes than Pages takes")

    work = tempfile.mkdtemp(prefix="hm-repos-test-")
    try:
        site = os.path.join(work, "public")
        os.makedirs(site)
        with open(os.path.join(site, "_redirects"), "w") as f:
            f.write("/download/hmailserver_amd64.deb https://gitlab.com/x 302")
        add_redirects(site, entries)
        with open(os.path.join(site, "_redirects")) as f:
            rules = parse_rules(f.read())
        expect(rules[0] == ("/download/hmailserver_amd64.deb", "https://gitlab.com/x", "302") and len(rules) == 1 + len(entries),
               "the feed's rules stay first and one rule is added per package")
        expect(all(status == "302" and target.startswith(REGISTRY) for _, target, status in rules[1:]),
               "every package rule is a 302 to the registry")
        status, where = pages_answer(site, "/apt/pool/main/h/hmailserver/hmailserver_6.3.5-1%7Edebian12_amd64.deb")
        expect(status == 302 and where.endswith("/6.3.5/hmailserver_6.3.5-1~debian12_amd64.deb"),
               "a path with ~ quoted as apt quotes it is matched decoded, as Pages matches it")
        try:
            add_redirects(site, entries)
            failures.append("a second rule for the same path is refused")
        except RepoError:
            pass
        many = [dict(entries[0], name="hmailserver_6.3.%d_amd64.deb" % i) for i in range(1001)]
        os.remove(os.path.join(site, "_redirects"))
        try:
            add_redirects(site, many)
            failures.append("more rules than Pages reads are refused")
        except RepoError as e:
            expect("Pages reads" in str(e), "the refusal says why: %s" % e)
        expect(not os.path.exists(os.path.join(site, "_redirects")), "nothing is written when the rules are refused")

        # No key: a refusal, exit 0, and the site as the feed wrote it.
        with open(os.path.join(site, "releases.json"), "w") as f:
            json.dump(releases, f)
        before = sorted(os.listdir(site))
        key = os.path.join(work, "key.asc")
        out = io.StringIO()
        saved = sys.stdout
        sys.stdout = out
        try:
            code = publish(site, os.path.join(work, "cache"), 0, public_key=key, environ={})
        finally:
            sys.stdout = saved
        expect(code == 0 and "REFUSED" in out.getvalue() and KEY_VARIABLE in out.getvalue(),
               "without the key it refuses, says why, and exits 0: %r" % out.getvalue())
        expect(sorted(os.listdir(site)) == before, "without the key nothing is added to the site")
        out = io.StringIO()
        sys.stdout = out
        try:
            code = publish(site, os.path.join(work, "cache"), 0, public_key=key, environ={KEY_VARIABLE: "c2VjcmV0"})
        finally:
            sys.stdout = saved
        expect(code == 0 and "REFUSED" in out.getvalue() and "not committed" in out.getvalue()
               and "not used" in out.getvalue(),
               "a secret with no committed public key is refused, said, and the feed still deploys: %r" % out.getvalue())
        expect(sorted(os.listdir(site)) == before, "with a secret but no committed key nothing is added to the site")
        for value in ("not base64 at all!", "===="):
            try:
                read_secret(value)
                failures.append("%r is refused as a key" % value)
            except RepoError:
                pass

        if all(shutil.which(t) for t in ("gpg", "dpkg-deb", "apt-ftparchive")):
            whole_build(work, failures, expect)
        else:
            say("self-test: gpg, dpkg-deb or apt-ftparchive is missing here, so no repository was built "
                "(the CI job has them)")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    for failure in failures:
        say("FAIL: " + failure)
    say("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def make_key(home, name):
    run(["gpg", "--homedir", home, "--batch", "--passphrase", "", "--quick-generate-key", name, "ed25519", "cert", "0"])
    out = run(["gpg", "--homedir", home, "--batch", "--with-colons", "--list-keys", name])
    fingerprint = [f[9] for f in colons(out) if f[0] == "fpr"][0]
    run(["gpg", "--homedir", home, "--batch", "--passphrase", "", "--quick-add-key", fingerprint, "ed25519", "sign", "2y"])
    return fingerprint


def whole_build(work, failures, expect):
    """A key and two .debs made here, an apt repository built from them, read
    back, and then broken three ways."""
    home = os.path.join(work, "gnupg")
    os.makedirs(home, mode=0o700)
    fingerprint = make_key(home, "hMailServer test repository <test@example.invalid>")
    public = os.path.join(work, "public.asc")
    with open(public, "wb") as f:
        f.write(run(["gpg", "--homedir", home, "--batch", "--armor", "--export", fingerprint]))
    secret = base64.b64encode(run(["gpg", "--homedir", home, "--batch", "--export-secret-subkeys"])).decode()

    cache = os.path.join(work, "cache")
    os.makedirs(cache)
    releases = []
    for version in ("6.3.5", "6.3.6"):
        assets = []
        for name, arch in (("hmailserver_%s_amd64.deb" % version, "amd64"), ("hmailserver_%s-1~debian13_amd64.deb" % version, "amd64")):
            deb = os.path.join(work, "debs", name)
            root = os.path.join(work, "debroot", name)
            os.makedirs(os.path.join(root, "DEBIAN"))
            with open(os.path.join(root, "DEBIAN", "control"), "w", newline="\n") as f:
                f.write("Package: hmailserver\nVersion: %s\nArchitecture: %s\nMaintainer: Test <test@example.invalid>\n"
                        "Description: test package\n" % (name.split("_")[1], arch))
            os.makedirs(os.path.dirname(deb), exist_ok=True)
            run(["dpkg-deb", "--root-owner-group", "-Zgzip", "--build", root, deb])
            digest = sha256_of(deb)
            shutil.copyfile(deb, os.path.join(cache, digest))
            assets.append({"name": name, "size": os.path.getsize(deb), "digest": "sha256:" + digest,
                           "browser_download_url": REGISTRY + "hmailserver/%s/%s" % (version, urllib.parse.quote(name))})
        releases.append({"tag_name": "v" + version, "prerelease": False, "draft": False, "assets": assets})

    def fresh():
        site = os.path.join(work, "site")
        shutil.rmtree(site, ignore_errors=True)
        os.makedirs(site)
        with open(os.path.join(site, "releases.json"), "w") as f:
            json.dump(releases, f)
        return site

    site = fresh()
    saved = sys.stdout
    sys.stdout = io.StringIO()
    try:
        code = publish(site, cache, 0, public_key=public, environ={KEY_VARIABLE: secret})
    finally:
        sys.stdout = saved
    expect(code == 0, "the repository is built")
    problems = check(site, public)
    expect(problems == [], "the repository reads back: %s" % problems)
    with open(os.path.join(site, "apt", "dists", "ubuntu26.04", "main", "binary-amd64", "Packages"), encoding="utf-8") as f:
        packages = f.read()
    expect(packages.count("Package: hmailserver") == 2 and "debian13" not in packages,
           "ubuntu26.04 holds the two generic .debs and nothing else")

    path = os.path.join(site, "apt", "dists", "debian13", "main", "binary-amd64", "Packages")
    with open(path, "a", encoding="utf-8") as f:
        f.write("\n")
    expect(any("Packages is not what Release says" in p for p in check(site, public)), "a changed Packages is caught")

    site = fresh()
    sys.stdout = io.StringIO()
    try:
        publish(site, cache, 0, public_key=public, environ={KEY_VARIABLE: secret})
    finally:
        sys.stdout = saved
    with open(os.path.join(site, "_redirects"), encoding="utf-8") as f:
        text = f.read()
    with open(os.path.join(site, "_redirects"), "w", encoding="utf-8") as f:
        f.write(text.replace("/6.3.6/hmailserver_6.3.6_amd64.deb 302", "/6.3.5/hmailserver_6.3.5_amd64.deb 302"))
    expect(any("hmailserver_6.3.6_amd64.deb goes to" in p for p in check(site, public)), "a rule sending a package elsewhere is caught")

    other_home = os.path.join(work, "gnupg-other")
    os.makedirs(other_home, mode=0o700)
    make_key(other_home, "Someone else <else@example.invalid>")
    other = base64.b64encode(run(["gpg", "--homedir", other_home, "--batch", "--export-secret-subkeys"])).decode()
    site = fresh()
    sys.stdout = io.StringIO()
    try:
        publish(site, cache, 0, public_key=public, environ={KEY_VARIABLE: other})
        failures.append("a secret key that is not the committed key's is refused")
    except RepoError as e:
        expect("no usable signing key" in str(e), "the refusal names the key: %s" % e)
    finally:
        sys.stdout = saved
    expect(not os.path.exists(os.path.join(site, "apt")), "nothing is written with the wrong key")


# ---------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--site", help="the site the pages job wrote (public/): add the repositories to it")
    parser.add_argument("--cache", default=".package-cache", help="where downloaded packages are kept, by SHA-256")
    parser.add_argument("--keep", type=int, default=0, help="keep the newest N stable versions only (default: every one)")
    parser.add_argument("--check", metavar="DIR", help="read the repositories in DIR back as apt and dnf would")
    parser.add_argument("--serve", metavar="DIR", help="serve DIR as GitLab Pages would, _redirects included")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--registry", default=REGISTRY, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.self_test:
            return self_test()
        if args.serve:
            serve(args.serve, args.bind, args.port)
            return 0
        if args.check:
            if not os.path.isdir(os.path.join(args.check, "apt")) and not os.path.isdir(os.path.join(args.check, "rpm")):
                say("%s has no repositories: nothing to check" % args.check)
                return 0
            problems = check(args.check, registry=args.registry)
            for problem in problems:
                say("PROBLEM: " + problem)
            say("check: %d problem(s)" % len(problems))
            return 1 if problems else 0
        if args.site:
            return publish(args.site, args.cache, args.keep, registry=args.registry)
        parser.print_help()
        return 2
    except RepoError as e:
        say("generate-package-repos: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
