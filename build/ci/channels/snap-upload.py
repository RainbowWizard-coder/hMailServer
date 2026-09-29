#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The snap-upload job of .gitlab/ci/channels.yml: the release's snap uploaded to the
Snap Store's edge and candidate channels; stable is promoted by hand
(hmailserver/docs/Channels.md, "Snap Store").

The snap is not a file of the release: the job snap-build (.gitlab/ci/snap.yml,
hmailserver/docs/Snap.md) makes it in the same pipeline, and this job takes it. The
contract between the two:

  - snap-build leaves snap/hmailserver_<version>_<arch>.snap (amd64, arm64) in its
    artifacts, <version> the release's;
  - and snap/hmailserver_<version>_<arch>.snap.source, one line "<SHA-256>
    hmailserver-<version>.tar.gz", when it built the snap from the release's
    source tarball.

This job, in order:

  1. Dormant: without the project's CI/CD variable SNAPCRAFT_STORE_CREDENTIALS
     (snapcraft export-login --snaps=hmailserver --acls
     package_access,package_push,package_update,package_release; protected,
     masked) it says "not configured" and exits 0.
  2. The release, as every channel takes it (build/ci/channels/release-files.py).
  3. Each snap held to the release: its name's version, and meta/snap.yaml's name and
     version inside it (unsquashfs); and what it was built from - the release's
     source tarball, downloaded here and verified against its Sigstore bundle as
     this project's pipeline for the tag, whose SHA-256 the .source line must name;
     or, without a .source line, this pipeline's own commit, which must be the
     release tag's (an annotated tag on master signed by the key master's
     .gitlab/allowed_signers lists: build/ci/release-chain.py's check). A snap of
     master after the release is refused.
  4. What the store's edge and candidate hold for each architecture, read from its
     public channel map (api.snapcraft.io, no credential): a release older than
     either is refused, since a snap channel is a pointer to a revision and every
     device following it would move back to the older one; and where both hold this
     release already, that snap is not uploaded again - a rebuilt snap is other
     bytes, which the store would take as a new revision of the same version.
  5. snapcraft upload --release=edge,candidate for each, which reads the credentials
     from its environment itself. A snap the store has already is said and passed.

Usage, from the root of a checkout, where snapcraft runs (the runner snap-build uses):
  python3 build/ci/channels/snap-upload.py [--snaps DIR] [--out DIR]
  python3 build/ci/channels/snap-upload.py --self-test
"""

import argparse
import glob
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

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

SNAP = re.compile(r"^hmailserver_(\d+\.\d+\.\d+)_(amd64|arm64)\.snap$")
CHANNELS = "edge,candidate"
# The store's public view of the snap: its channel map, anonymously (the snapd API).
STORE_INFO = "https://api.snapcraft.io/v2/snaps/info/hmailserver?fields=version,revision"


def say(line=""):
    print(line)
    sys.stdout.flush()


def snap_metadata(path, run=subprocess.run):
    """(name, version) from meta/snap.yaml inside a snap."""
    answer = run(["unsquashfs", "-cat", path, "meta/snap.yaml"], capture_output=True, text=True)
    if answer.returncode != 0:
        raise Refused("unsquashfs could not read %s's meta/snap.yaml: %s" % (os.path.basename(path),
                                                                           (answer.stderr or "").strip()[-200:]))
    name = re.search(r"^name:\s*['\"]?([^'\"\s]+)", answer.stdout, re.MULTILINE)
    version = re.search(r"^version:\s*['\"]?([^'\"\s]+)", answer.stdout, re.MULTILINE)
    return (name.group(1) if name else None, version.group(1) if version else None)


def store_channels(fix=None, http=None):
    """{(architecture, risk): version} of the store's latest track: what each channel
    holds now, {} for a snap the store has never released."""
    url = (fix.get("snap_store_info") if fix else None) or STORE_INFO
    status, _, body = (http or rf.http)(url, headers={"Snap-Device-Series": "16", "Accept": "application/json"})
    if status == 404:
        return {}
    try:
        answer = json.loads(body.decode("utf-8")) if status == 200 else None
    except ValueError:
        answer = None
    if not isinstance(answer, dict):
        raise Refused("the Snap Store's channel map could not be read (HTTP %s): nothing was uploaded" % status)
    held = {}
    for entry in answer.get("channel-map") or []:
        channel = entry.get("channel") or {}
        if channel.get("track", "latest") == "latest":
            held[(channel.get("architecture"), channel.get("risk"))] = str(entry.get("version"))
    return held


def against_store(version, arch, held):
    """True when the store's edge and candidate for arch hold this release already;
    Refused when either holds a newer one, or one that is not a release version."""
    have = dict((risk, held.get((arch, risk))) for risk in CHANNELS.split(","))
    for risk, there in have.items():
        if there is None:
            continue
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", there):
            raise Refused("the store's %s for %s holds %s, which is no release version: decide by hand what replaces it"
                          % (risk, arch, there))
        if tuple(int(p) for p in there.split(".")) > tuple(int(p) for p in version.split(".")):
            raise Refused("the store's %s for %s holds %s, newer than the release %s: a channel never goes back, and"
                          " every device following it would. Nothing was uploaded." % (risk, arch, there, version))
    return all(there == version for there in have.values())


def built_from_tag(version, environ, run=subprocess.run, root=ROOT):
    """True when this pipeline's commit is the release tag's, as the release job
    checks a tag."""
    commit = environ.get("CI_COMMIT_SHA") or ""
    chain = load("release_chain", os.path.join(root, "build", "ci", "release-chain.py"))
    problems = chain.check_tag(root, "v" + version, commit, run=run)
    return not problems, problems


def snap_upload(out, snaps="snap", environ=None, run=subprocess.run, http=None):
    environ = os.environ if environ is None else environ
    fix = rf.fixture(environ)
    if not environ.get("SNAPCRAFT_STORE_CREDENTIALS"):
        say("not configured: SNAPCRAFT_STORE_CREDENTIALS is not set, so nothing was uploaded.")
        say("The owner registers the name hmailserver on the Snap Store and runs snapcraft export-login for it; the")
        say("result is the protected, masked CI/CD variable SNAPCRAFT_STORE_CREDENTIALS (hmailserver/docs/Channels.md).")
        return 0

    version, where = rf.release_version(environ, fix)
    say("the release: %s (from %s)" % (version, where))
    found = sorted(glob.glob(os.path.join(snaps, "hmailserver_*.snap")))
    mine = [p for p in found if SNAP.match(os.path.basename(p)) and SNAP.match(os.path.basename(p)).group(1) == version]
    if not mine:
        raise Refused("snap-build left no snap/hmailserver_%s_<amd64|arm64>.snap (it left: %s)"
                      % (version, ", ".join(os.path.basename(p) for p in found) or "nothing"))
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)

    tarball = "hmailserver-%s.tar.gz" % version
    verified = None
    tagged = None
    for path in mine:
        name, inside = snap_metadata(path, run)
        if name != "hmailserver" or inside != version:
            raise Refused("%s's meta/snap.yaml says %s %s, not hmailserver %s" % (os.path.basename(path), name, inside,
                                                                                version))
        source = path + ".source"
        if os.path.isfile(source):
            with open(source, encoding="utf-8") as f:
                line = f.read().split()
            if len(line) != 2 or line[1] != tarball or not re.match(r"^[0-9a-f]{64}$", line[0]):
                raise Refused("%s is not one line '<SHA-256> %s'" % (os.path.basename(source), tarball))
            if verified is None:
                cosign = rf.tool_path("cosign", out, environ)
                record = rf.verified_files(version, [tarball], os.path.join(out, "release"), cosign, fix)
                rf.write_record(record, version, where, os.path.join(out, "verification.json"))
                verified = record[tarball]
            if line[0] != verified["sha256"]:
                raise Refused("%s was built from a %s of SHA-256 %s, and the release's is %s"
                              % (os.path.basename(path), tarball, line[0], verified["sha256"]))
            say("%s: built from v%s's tarball, verified as %s" % (os.path.basename(path), version, verified["identity"]))
        else:
            if tagged is None:
                tagged = built_from_tag(version, environ, run)
            if not tagged[0]:
                raise Refused("%s says nothing of its source, and this pipeline's commit is not v%s's:\n  %s"
                              % (os.path.basename(path), version, "\n  ".join(tagged[1])))
            say("%s: built in this pipeline, whose commit is v%s's" % (os.path.basename(path), version))

    held = store_channels(fix, http)
    pending = []
    for path in mine:
        arch = SNAP.match(os.path.basename(path)).group(2)
        if against_store(version, arch, held):
            say("%s: the store's edge and candidate hold %s for %s already" % (os.path.basename(path), version, arch))
        else:
            pending.append(path)
    if not pending:
        say("Nothing to upload.")
        return 0

    if not shutil.which("snapcraft", path=environ.get("PATH")) and not (fix and fix.get("snapcraft")):
        raise Refused("there is no snapcraft here: snap-upload runs where snap-build does (snap install snapcraft"
                      " --classic, from the Snap Store)")
    snapcraft = (fix.get("snapcraft") if fix else None) or "snapcraft"
    for path in pending:
        answer = run([snapcraft, "upload", "--release=" + CHANNELS, path], capture_output=True, text=True,
                     env=dict(environ))
        said = ((answer.stdout or "") + (answer.stderr or "")).strip()
        if answer.returncode != 0:
            if re.search(r"already been uploaded|duplicate", said, re.IGNORECASE):
                say("%s: the store has it already" % os.path.basename(path))
                continue
            raise Refused("snapcraft upload of %s failed: %s" % (os.path.basename(path), said[-400:]))
        say("%s: %s" % (os.path.basename(path), said.splitlines()[-1] if said else "uploaded"))
    say("PUBLISHED: hmailserver %s to %s (%s); stable is promoted by hand (snapcraft release hmailserver <revision>"
        " stable)." % (version, CHANNELS, ", ".join(SNAP.match(os.path.basename(p)).group(2) for p in pending)))
    return 0


# ---------------------------------------------------------------- self-test

class Answer:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


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

    work = tempfile.mkdtemp(prefix="hm-snap-test-")
    held_say = globals()["say"]
    held = (rf.release_version, rf.tool_path, rf.verified_files)
    try:
        said = []
        globals()["say"] = said.append
        expect("without SNAPCRAFT_STORE_CREDENTIALS: 'not configured', exit 0, nothing read",
               snap_upload(os.path.join(work, "o"), os.path.join(work, "none"), {}, run=None) == 0
               and said[0].startswith("not configured"))
        globals()["say"] = lambda line="": None
        rf.release_version = lambda environ, fix: ("9.9.9", "HM_CHANNEL_VERSION")
        rf.tool_path = lambda tool, work, environ=None: "cosign"
        sha = "ab" * 32
        rf.verified_files = lambda version, names, into, cosign, fix=None, run=None: {names[0]: {
            "path": "x", "sha256": sha, "size": 1, "identity": rf.identities("9.9.9")[1], "repository_id": "86730559"}}
        snaps = os.path.join(work, "snap")
        os.makedirs(snaps)
        environ = {"SNAPCRAFT_STORE_CREDENTIALS": "c2VjcmV0", "PATH": work}

        # The store's channel map, in memory: never released, or holding these.
        def store(held, status=200):
            entries = [{"channel": {"architecture": a, "risk": r, "track": "latest", "name": r}, "version": v,
                        "revision": 7} for (a, r), v in held.items()]

            def http(url, **kwargs):
                return status, {}, json.dumps({"channel-map": entries}).encode()
            return http

        def never(url, **kwargs):
            return 404, {}, b'{"error-list": [{"code": "resource-not-found"}]}'
        expect("the store's channel map read, the latest track alone", store_channels(http=store({
            ("amd64", "candidate"): "9.9.9", ("arm64", "edge"): "9.9.8"})) == {
            ("amd64", "candidate"): "9.9.9", ("arm64", "edge"): "9.9.8"} and store_channels(http=never) == {})
        refused("a channel map the store does not give", lambda: store_channels(http=store({}, status=503)),
                "could not be read")
        refused("no snap of the release", lambda: snap_upload(os.path.join(work, "a"), snaps, environ, http=never), "left no snap")
        for arch in ("amd64", "arm64"):
            with open(os.path.join(snaps, "hmailserver_9.9.9_%s.snap" % arch), "wb") as f:
                f.write(b"hsqs")
        calls = []

        def run(version="9.9.9", upload=0, said="Revision 12 created for 'hmailserver'"):
            def answer(command, **kwargs):
                calls.append((command, kwargs.get("env")))
                if command[0] == "unsquashfs":
                    return Answer(0, "name: hmailserver\nversion: '%s'\nbase: core24\n" % version)
                return Answer(upload, said)
            return answer
        refused("a snap whose meta/snap.yaml is another version", lambda: snap_upload(
            os.path.join(work, "b"), snaps, environ, http=never, run=run(version="9.9.8")), "not hmailserver 9.9.9")
        for arch in ("amd64", "arm64"):
            with open(os.path.join(snaps, "hmailserver_9.9.9_%s.snap.source" % arch), "w") as f:
                f.write("%s  hmailserver-9.9.9.tar.gz\n" % ("cd" * 32))
        refused("a snap built from another tarball than the release's verified one", lambda: snap_upload(
            os.path.join(work, "c"), snaps, environ, http=never, run=run()), "the release's is " + sha)
        for arch in ("amd64", "arm64"):
            with open(os.path.join(snaps, "hmailserver_9.9.9_%s.snap.source" % arch), "w") as f:
                f.write("%s  hmailserver-9.9.9.tar.gz\n" % sha)
        refused("no snapcraft on the runner", lambda: snap_upload(os.path.join(work, "d"), snaps, environ, http=never, run=run()),
                "no snapcraft here")
        with open(os.path.join(work, "snapcraft" + (".exe" if os.name == "nt" else "")), "w") as f:
            f.write("")
        os.chmod(os.path.join(work, "snapcraft" + (".exe" if os.name == "nt" else "")), 0o755)
        del calls[:]
        code = snap_upload(os.path.join(work, "e"), snaps, environ, http=never, run=run())
        uploads = [c for c in calls if c[0][0] == "snapcraft"]
        expect("verified source, then snapcraft upload --release=edge,candidate of each, the credentials in its"
               " environment alone", code == 0 and [c[0] for c in uploads] == [
                   ["snapcraft", "upload", "--release=edge,candidate", os.path.join(snaps, "hmailserver_9.9.9_%s.snap" % a)]
                   for a in ("amd64", "arm64")]
               and all("c2VjcmV0" not in " ".join(c[0]) and c[1].get("SNAPCRAFT_STORE_CREDENTIALS") == "c2VjcmV0"
                       for c in uploads))
        del calls[:]
        expect("a snap the store has already: passed", snap_upload(os.path.join(work, "f"), snaps, environ, http=never, run=run(
            upload=2, said="Error: A file with this exact same content has already been uploaded")) == 0)

        # What the store's channels hold already.
        del calls[:]
        refused("the store's candidate holding a newer release: refused, a channel never goes back", lambda: snap_upload(
            os.path.join(work, "h"), snaps, environ, http=store({("arm64", "candidate"): "9.9.10"}), run=run()),
            "never goes back")
        expect("  ... and nothing was uploaded", not [c for c in calls if c[0][0] == "snapcraft"])
        refused("a channel holding what is no release version", lambda: snap_upload(
            os.path.join(work, "i"), snaps, environ, http=store({("amd64", "edge"): "9.9.9-dev"}), run=run()),
            "no release version")
        del calls[:]
        both = dict(((a, r), "9.9.9") for a in ("amd64", "arm64") for r in ("edge", "candidate"))
        expect("edge and candidate holding this release for both: nothing uploaded again", snap_upload(
            os.path.join(work, "j"), snaps, environ, http=store(both), run=run()) == 0
            and not [c for c in calls if c[0][0] == "snapcraft"])
        del calls[:]
        code = snap_upload(os.path.join(work, "k"), snaps, environ, run=run(), http=store({
            ("amd64", "edge"): "9.9.9", ("amd64", "candidate"): "9.9.9", ("arm64", "edge"): "9.9.9",
            ("arm64", "candidate"): "9.9.8"}))
        expect("  ... and where one channel holds an older one, that architecture's snap alone uploaded",
               code == 0 and [c[0][-1] for c in calls if c[0][0] == "snapcraft"]
               == [os.path.join(snaps, "hmailserver_9.9.9_arm64.snap")])
        for arch in ("amd64", "arm64"):
            os.remove(os.path.join(snaps, "hmailserver_9.9.9_%s.snap.source" % arch))
        held_tag = globals()["built_from_tag"]
        globals()["built_from_tag"] = lambda version, environ, run=None: (False, ["v9.9.9 points at 1111, and this pipeline"
                                                                                  " built 2222"])
        try:
            refused("no source line, and a pipeline whose commit is not the tag's (master after the release)",
                    lambda: snap_upload(os.path.join(work, "g"), snaps, environ, http=never, run=run()), "is not v9.9.9's")
        finally:
            globals()["built_from_tag"] = held_tag
    finally:
        globals()["say"] = held_say
        rf.release_version, rf.tool_path, rf.verified_files = held
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
    parser.add_argument("--snaps", default="snap")
    parser.add_argument("--out", default="snap-upload")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    try:
        return snap_upload(os.path.abspath(arguments.out), arguments.snaps)
    except Refused as error:
        say("REFUSED: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
