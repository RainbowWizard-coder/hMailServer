#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The ppa-upload job of .gitlab/ci/channels.yml: the release's Debian source package
uploaded to the project's Launchpad PPA, which builds it for each Ubuntu release
named (hmailserver/docs/Channels.md, "Ubuntu PPA").

  1. Dormant: without the project's CI/CD variable LAUNCHPAD_SIGNING_KEY (base64 of
     the upload key's secret key, the OpenPGP key registered on the Launchpad
     account that may upload to the PPA; protected, masked) it says "not configured"
     and exits 0. LAUNCHPAD_SIGNING_PASSPHRASE, when the key has one.
  2. The release, as every channel takes it (build/ci/channels/release-files.py).
  3. The release's source tarball, downloaded and verified against its Sigstore
     bundle as this project's pipeline for the tag, before anything is built.
  4. The source package made as the tarball's own Debian packaging says -
     hmailserver/source/Server/platform/packaging/debian-src/debian, taken from the
     verified tarball and never from this checkout (the source package of
     hmailserver/docs/SourcePackages.md, build/ci/packages/srcpkg-deb.sh): the
     tarball repacked by mk-origtargz with debian/copyright's Files-Excluded into
     hmailserver_<v>+ds.orig.tar.gz, or the PPA's own copy of that file when an
     earlier upload of this version sent it (Launchpad keeps one per version, and
     its contents are held to the repack's file by file); then for each Ubuntu
     release in LAUNCHPAD_SERIES (default "noble resolute", 24.04 and 26.04) that the
     PPA does not have the version for, a changelog entry for that release, version
     <the package's>~<release>1, dpkg-buildpackage -S, and debsign with the key.
     Every upload carries the orig (-sa) while the PPA has none, and none (-sd) once
     it has one (series_flags): Launchpad takes a file it already holds when the
     bytes are the same and refuses it when they differ (its "Troubleshoot package
     upload errors"), and an upload that only names the orig is refused while the
     PPA does not hold it - so each upload stands alone, whatever order Launchpad
     takes them in and whether an earlier one was refused.
  5. dput to ppa.launchpad.net, which takes the upload on its signature alone.
     Launchpad mails its answer to the key's address and shows it on the PPA's page;
     the job reads the PPA back after a while and says what it lists.

Usage, from the root of a checkout, in Debian 13 with devscripts, dpkg-dev, dput and
gnupg:
  python3 build/ci/channels/ppa-upload.py [--out DIR]
  python3 build/ci/channels/ppa-upload.py --self-test
"""

import argparse
import base64
import datetime
import email.utils
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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def load(name, path):
    """A module of this repository by its path, once per process: every channel script
    shares one release-files.py, and so one Refused, whichever loads it first."""
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


rf = load("release_files", os.path.join(HERE, "release-files.py"))
Refused = rf.Refused

# Where the tarball keeps its Debian packaging (W2.2's source package).
DEBIAN_DIR = "hmailserver/source/Server/platform/packaging/debian-src/debian"
DEFAULT_PPA = "hmailserver/hmailserver"
DEFAULT_SERIES = "noble resolute"
LAUNCHPAD_API = "https://api.launchpad.net/1.0"
LAUNCHPAD = "https://launchpad.net"


def say(line=""):
    print(line)
    sys.stdout.flush()


# ---------------------------------------------------------------- versions and the changelog

def package_version(changelog):
    """(source name, version, distribution) of a debian/changelog's first entry."""
    match = re.match(r"^(\S+) \(([^)]+)\) ([^;]+);", changelog)
    if not match:
        raise Refused("the tarball's debian/changelog does not open with an entry")
    return match.group(1), match.group(2), match.group(3).strip()


def upstream_of(version):
    return version.rsplit("-", 1)[0].split(":", 1)[-1]


def series_version(version, series, rebuild=1):
    """The version of the upload for one Ubuntu release, <version>~<release><n>: the
    tilde sorts it before the package's own <v>+ds-1, so a package Ubuntu itself
    carries one day supersedes the PPA's, and each Ubuntu release's after the one
    before it (noble < resolute), so an upgrade of Ubuntu upgrades the package too."""
    return "%s~%s%d" % (version, series, rebuild)


def changelog_entry(name, version, series, maintainer, when):
    return ("%s (%s) %s; urgency=medium\n\n"
            "  * The release's source package, for Ubuntu's %s, uploaded to the project's PPA\n"
            "    (hmailserver/docs/Channels.md).\n\n"
            " -- %s  %s\n\n") % (name, version, series, series, maintainer, email.utils.format_datetime(when))


# ---------------------------------------------------------------- the PPA

def ppa_parts(value):
    value = (value or DEFAULT_PPA).strip().lstrip("~")
    if not re.match(r"^[a-z0-9][a-z0-9+.\-]*/[a-z0-9][a-z0-9+.\-]*$", value):
        raise Refused("LAUNCHPAD_PPA is %r: it names a PPA as <owner>/<name>" % value)
    return value.split("/")


def published(owner, name, source, fix=None, http_json=None):
    """{(series, version)} the PPA holds of the source, in any state that keeps the
    version spent (Launchpad refuses a version it has had, even deleted)."""
    http_json = http_json or rf.http_json
    url = ("%s/~%s/+archive/ubuntu/%s?ws.op=getPublishedSources&source_name=%s&exact_match=true"
           % (LAUNCHPAD_API, owner, name, source))
    held = set()
    while url:
        status, answer = http_json(url)
        if status == 404:
            raise Refused("Launchpad has no PPA ~%s/ubuntu/%s: the owner creates it (hmailserver/docs/Channels.md)"
                          % (owner, name))
        if status != 200 or not isinstance(answer, dict):
            raise Refused("Launchpad answered %s for the PPA's sources" % status)
        for entry in answer.get("entries") or []:
            held.add((entry.get("distro_series_link", "").rstrip("/").rsplit("/", 1)[-1], entry.get("source_package_version")))
        url = answer.get("next_collection_link")
    return held


def tar_contents(path):
    """{member name without its first directory: SHA-256} of a tarball's files."""
    contents = {}
    with tarfile.open(path) as archive:
        for info in archive.getmembers():
            if info.isfile():
                contents[info.name.split("/", 1)[-1]] = hashlib.sha256(archive.extractfile(info).read()).hexdigest()
            elif info.issym():
                contents[info.name.split("/", 1)[-1]] = "-> " + info.linkname
    return contents


# ---------------------------------------------------------------- the job

def series_flags(wanted, ppa_has_orig):
    """dpkg-buildpackage's orig flag for each release's upload, in order: the orig
    carried by every one (-sa) while the PPA holds none, so that no upload rests on
    another having been taken first, and by none (-sd) once the PPA holds it, whose
    bytes the uploads are then built against."""
    return [(s, "-sd" if ppa_has_orig else "-sa") for s in wanted]


def ppa_upload(out, environ=None, run=subprocess.run):
    environ = os.environ if environ is None else environ
    fix = rf.fixture(environ)
    if not environ.get("LAUNCHPAD_SIGNING_KEY"):
        say("not configured: LAUNCHPAD_SIGNING_KEY is not set, so nothing was uploaded.")
        say("The owner makes the Launchpad team and PPA and registers the maintainers' upload key; its secret")
        say("key, base64, is the protected, masked CI/CD variable LAUNCHPAD_SIGNING_KEY")
        say("(hmailserver/docs/Channels.md, 'Ubuntu PPA').")
        return 0
    owner, ppa = ppa_parts(environ.get("LAUNCHPAD_PPA"))
    series = (environ.get("LAUNCHPAD_SERIES") or DEFAULT_SERIES).split()
    if not series or not all(re.match(r"^[a-z]+$", s) for s in series):
        raise Refused("LAUNCHPAD_SERIES is %r: Ubuntu releases by their names (noble resolute)" % environ.get("LAUNCHPAD_SERIES"))

    version, where = rf.release_version(environ, fix)
    say("the release: %s (from %s); the PPA ~%s/ubuntu/%s; for %s" % (version, where, owner, ppa, ", ".join(series)))
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    tarball = "hmailserver-%s.tar.gz" % version
    cosign = rf.tool_path("cosign", out, environ)
    record = rf.verified_files(version, [tarball], os.path.join(out, "release"), cosign, fix)
    rf.write_record(record, version, where, os.path.join(out, "verification.json"))
    verified = record[tarball]["path"]

    # The tarball's own packaging.
    with tarfile.open(verified) as archive:
        top = "hmailserver-%s" % version
        members = [m for m in archive.getmembers() if m.name.startswith("%s/%s/" % (top, DEBIAN_DIR))]
        if not members:
            raise Refused("v%s's tarball has no %s: the first release with the Debian source package is the one after"
                          " W2.2 is folded" % (version, DEBIAN_DIR))
        packaging = os.path.join(out, "packaging")
        for member in members:
            member.name = member.name[len("%s/%s/" % (top, DEBIAN_DIR)):]
            if member.name:
                archive.extract(member, packaging, filter="data") if hasattr(tarfile, "data_filter") else archive.extract(member, packaging)
    with open(os.path.join(packaging, "changelog"), encoding="utf-8") as f:
        changelog = f.read()
    source, base_version, _ = package_version(changelog)
    upstream = upstream_of(base_version)
    if source != "hmailserver" or upstream.split("+", 1)[0] != version:
        raise Refused("the tarball's debian/changelog names %s %s, not hmailserver at %s" % (source, base_version, version))

    if fix and "ppa_published" in fix.data:
        have = set(tuple(pair) for pair in fix.get("ppa_published"))
    else:
        have = published(owner, ppa, source, fix)
    wanted = [s for s in series if (s, series_version(base_version, s)) not in have]
    for s in series:
        if s not in wanted:
            say("the PPA has %s %s for %s already" % (source, series_version(base_version, s), s))
    if not wanted:
        say("Nothing to upload.")
        return 0

    # The key, in a GnuPG home of the job's own.
    home = tempfile.mkdtemp(prefix="hm-ppa-gnupg-")
    try:
        # Owner only, as GnuPG requires of its home.
        os.chmod(home, 0o700)  # nosemgrep
        gpg_env = dict(environ, GNUPGHOME=home)
        passphrase = environ.get("LAUNCHPAD_SIGNING_PASSPHRASE") or ""
        with open(os.path.join(home, "gpg.conf"), "w") as f:
            f.write("batch\npinentry-mode loopback\n")
            if passphrase:
                f.write("passphrase-file %s\n" % os.path.join(home, "passphrase"))
        if passphrase:
            descriptor = os.open(os.path.join(home, "passphrase"), os.O_WRONLY | os.O_CREAT, 0o600)
            with os.fdopen(descriptor, "w") as f:
                f.write(passphrase)
        try:
            key = base64.b64decode(environ["LAUNCHPAD_SIGNING_KEY"].strip(), validate=True)
        except ValueError:
            raise Refused("LAUNCHPAD_SIGNING_KEY is not base64 (base64 -w0 of gpg --export-secret-keys)")
        imported = run(["gpg", "--import"], input=key, capture_output=True, env=gpg_env)
        key = None
        if imported.returncode != 0:
            raise Refused("gpg could not import LAUNCHPAD_SIGNING_KEY")
        listed = run(["gpg", "--list-secret-keys", "--with-colons"], capture_output=True, text=True, env=gpg_env).stdout
        fingerprints = [l.split(":")[9] for l in listed.splitlines() if l.startswith("fpr:")]
        uids = [l.split(":")[9] for l in listed.splitlines() if l.startswith("uid:")]
        if not fingerprints or not uids:
            raise Refused("LAUNCHPAD_SIGNING_KEY holds no secret key with a user id")
        fingerprint, maintainer = fingerprints[0], uids[0]
        say("signing as %s, %s" % (fingerprint, maintainer))

        # The orig: the PPA's, when an earlier upload of this version sent one, held
        # to this tarball's repack; otherwise the repack.
        work = os.path.join(out, "build")
        os.makedirs(work)
        repack = run(["mk-origtargz", "--package", source, "--version", version, "--repack-suffix", "+ds",
                      "--compression", "gzip", "--copyright-file", os.path.join(packaging, "copyright"),
                      "--directory", work, "--repack", verified], capture_output=True, text=True, env=gpg_env)
        orig = os.path.join(work, "%s_%s.orig.tar.gz" % (source, upstream))
        if repack.returncode != 0 or not os.path.isfile(orig):
            raise Refused("mk-origtargz could not repack the tarball: %s" % (repack.stderr or repack.stdout).strip()[-400:])
        spent = [v for s, v in have if v and upstream_of(v) == upstream]
        if spent:
            url = "%s/~%s/+archive/ubuntu/%s/+sourcefiles/%s/%s/%s" % (LAUNCHPAD, owner, ppa, source, spent[0],
                                                                         os.path.basename(orig))
            theirs = orig + ".launchpad"
            rf.download(url, theirs)
            if tar_contents(theirs) != tar_contents(orig):
                raise Refused("the PPA's %s is not this release's tarball repacked, file by file: nothing was uploaded"
                              % os.path.basename(orig))
            os.replace(theirs, orig)
            say("the PPA has %s already (from %s): uploading against it" % (os.path.basename(orig), spent[0]))

        unpacked = os.path.join(work, "%s-%s" % (source, upstream))
        uploads = []
        for s, flag in series_flags(wanted, bool(spent)):
            if os.path.exists(unpacked):
                shutil.rmtree(unpacked)
            with tarfile.open(orig) as archive:
                inner = archive.getmembers()[0].name.split("/", 1)[0]
                archive.extractall(work, filter="data") if hasattr(tarfile, "data_filter") else archive.extractall(work)
            if os.path.join(work, inner) != unpacked:
                os.rename(os.path.join(work, inner), unpacked)
            shutil.copytree(packaging, os.path.join(unpacked, "debian"))
            upload_version = series_version(base_version, s)
            with open(os.path.join(unpacked, "debian", "changelog"), "w", encoding="utf-8", newline="\n") as f:
                f.write(changelog_entry(source, upload_version, s, maintainer, datetime.datetime.now(datetime.timezone.utc))
                        + changelog)
            built = run(["dpkg-buildpackage", "-S", "-d", "-nc", "-us", "-uc", flag], cwd=unpacked, capture_output=True,
                        text=True, env=gpg_env)
            if built.returncode != 0:
                raise Refused("dpkg-buildpackage -S for %s failed: %s" % (s, (built.stderr or built.stdout).strip()[-600:]))
            changes = os.path.join(work, "%s_%s_source.changes" % (source, upload_version))
            signed = run(["debsign", "-k" + fingerprint, "--re-sign", changes], capture_output=True, text=True, env=gpg_env)
            if signed.returncode != 0:
                raise Refused("debsign failed for %s: %s" % (s, (signed.stderr or signed.stdout).strip()[-400:]))
            say("built and signed: %s" % os.path.basename(changes))
            uploads.append((s, upload_version, changes))

        dput_cf = os.path.join(home, "dput.cf")
        target = fix.get("ppa_incoming") if fix else None
        with open(dput_cf, "w") as f:
            if target:
                f.write("[ppa-hmailserver]\nmethod = local\nincoming = %s\nallow_unsigned_uploads = 0\n" % target)
            else:
                f.write("[ppa-hmailserver]\nfqdn = ppa.launchpad.net\nmethod = ftp\nincoming = ~%s/ubuntu/%s/\n"
                        "login = anonymous\nallow_unsigned_uploads = 0\n" % (owner, ppa))
        for s, upload_version, changes in uploads:
            sent = run(["dput", "--config", dput_cf, "ppa-hmailserver", changes], capture_output=True, text=True, env=gpg_env)
            if sent.returncode != 0:
                raise Refused("dput of %s failed: %s" % (os.path.basename(changes), (sent.stderr or sent.stdout).strip()[-400:]))
            say("uploaded: %s %s for %s" % (source, upload_version, s))
        for name in os.listdir(work):
            if name.endswith((".dsc", ".changes", ".buildinfo", ".debian.tar.xz")):
                shutil.copy2(os.path.join(work, name), out)
    finally:
        shutil.rmtree(home, ignore_errors=True)

    if not target:
        # Launchpad takes an upload in a few minutes and answers by mail; asked here too.
        for attempt in range(10):
            time.sleep(30)
            have = published(owner, ppa, source)
            if all((s, v) in have for s, v, _ in uploads):
                break
        missing = [v for s, v, _ in uploads if (s, v) not in have]
        if missing:
            say("note: the PPA does not list %s yet; Launchpad mails its answer to %s" % (", ".join(missing), maintainer))
    say("PUBLISHED: %s for %s to ~%s/ubuntu/%s; Launchpad builds each." % (source, ", ".join(wanted), owner, ppa))
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
            expect("%s (%s)" % (what, str(error).splitlines()[0][:110]), needle in str(error))
            return
        expect(what + ": not refused", False)

    changelog = ("hmailserver (9.9.9+ds-1) unstable; urgency=medium\n\n  * x\n\n"
                 " -- A <a@example.org>  Mon, 28 Sep 2026 20:00:00 +0000\n")
    expect("the changelog's first entry read", package_version(changelog) == ("hmailserver", "9.9.9+ds-1", "unstable"))
    expect("the upstream version of 9.9.9+ds-1", upstream_of("9.9.9+ds-1") == "9.9.9+ds")
    expect("each Ubuntu release's version sorts before the package's own and after the release before it",
           series_version("9.9.9+ds-1", "noble") == "9.9.9+ds-1~noble1" and "noble" < "resolute")
    entry = changelog_entry("hmailserver", "9.9.9+ds-1~noble1", "noble", "Upload Key <ppa@example.org>",
                            datetime.datetime(2026, 10, 2, 12, 0, tzinfo=datetime.timezone.utc))
    expect("a changelog entry dpkg-parsechangelog reads", entry.startswith("hmailserver (9.9.9+ds-1~noble1) noble; urgency=medium")
           and " -- Upload Key <ppa@example.org>  Fri, 02 Oct 2026 12:00:00 +0000\n" in entry)
    expect("with no orig in the PPA, every release's upload carries it",
           series_flags(["noble", "resolute"], False) == [("noble", "-sa"), ("resolute", "-sa")])
    expect("  ... and with the PPA's own, none does",
           series_flags(["noble", "resolute"], True) == [("noble", "-sd"), ("resolute", "-sd")])
    expect("the PPA named by owner and name", ppa_parts(None) == ["hmailserver", "hmailserver"]
           and ppa_parts("~someone/test-ppa") == ["someone", "test-ppa"])
    refused("a PPA named otherwise", lambda: ppa_parts("ppa:hmailserver"), "<owner>/<name>")

    pages = {"first": (200, {"entries": [{"distro_series_link": "https://api.launchpad.net/1.0/ubuntu/noble",
                                          "source_package_version": "9.9.9+ds-1~noble1"}],
                             "next_collection_link": "second"}),
             "second": (200, {"entries": [{"distro_series_link": "https://api.launchpad.net/1.0/ubuntu/resolute",
                                           "source_package_version": "9.9.8+ds-1~resolute1"}]})}
    expect("what the PPA has, over two pages", published("o", "p", "hmailserver", http_json=lambda url: pages[
        "second" if url == "second" else "first"]) == {("noble", "9.9.9+ds-1~noble1"), ("resolute", "9.9.8+ds-1~resolute1")})
    refused("a PPA that does not exist", lambda: published("o", "p", "hmailserver", http_json=lambda url: (404, None)),
            "the owner creates it")

    work = tempfile.mkdtemp(prefix="hm-ppa-test-")
    try:
        def tarball(path, files):
            with tarfile.open(path, "w:gz") as archive:
                for name, data in files.items():
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
        tarball(os.path.join(work, "a.tar.gz"), {"hmailserver-9.9.9/x": b"1", "hmailserver-9.9.9/y": b"2"})
        tarball(os.path.join(work, "b.tar.gz"), {"hmailserver-9.9.9+ds.orig/x": b"1", "hmailserver-9.9.9+ds.orig/y": b"2"})
        tarball(os.path.join(work, "c.tar.gz"), {"hmailserver-9.9.9/x": b"1", "hmailserver-9.9.9/y": b"3"})
        expect("two origs with the same files under another directory are the same contents",
               tar_contents(os.path.join(work, "a.tar.gz")) == tar_contents(os.path.join(work, "b.tar.gz")))
        expect("  ... and one file changed is not", tar_contents(os.path.join(work, "a.tar.gz"))
               != tar_contents(os.path.join(work, "c.tar.gz")))

        said = []
        held_say = globals()["say"]
        globals()["say"] = said.append
        try:
            expect("without LAUNCHPAD_SIGNING_KEY: 'not configured', exit 0, nothing read",
                   ppa_upload(os.path.join(work, "o"), {}, run=None) == 0 and said[0].startswith("not configured"))
        finally:
            globals()["say"] = held_say
        calls = []
        held = (rf.release_version, rf.tool_path, rf.verified_files)
        try:
            rf.release_version = lambda environ, fix: ("9.9.9", "HM_CHANNEL_VERSION")
            rf.tool_path = lambda tool, work, environ=None: "cosign"

            def unverified(*args, **kwargs):
                raise Refused("hmailserver-9.9.9.tar.gz does not verify as signed by this project's pipeline for v9.9.9")
            rf.verified_files = unverified
            globals()["say"] = lambda line="": None
            refused("with a key, a tarball that does not verify: refused before gpg, a build or dput",
                    lambda: ppa_upload(os.path.join(work, "p"), {"LAUNCHPAD_SIGNING_KEY": "a2V5"},
                                       run=lambda *a, **k: calls.append(a)), "does not verify")
            expect("  ... nothing was run", calls == [])
        finally:
            globals()["say"] = held_say
            rf.release_version, rf.tool_path, rf.verified_files = held
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("")
    if failures:
        print("%d self-test case(s) failed" % len(failures))
        return 1
    print("Every self-test case passed.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--out", default="ppa-upload")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    try:
        return ppa_upload(os.path.abspath(arguments.out))
    except Refused as error:
        say("REFUSED: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
