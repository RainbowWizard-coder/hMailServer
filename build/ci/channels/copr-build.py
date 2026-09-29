#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Fedora Copr: the release built for Fedora and Enterprise Linux in the Copr project
hmailserver (hmailserver/docs/Channels.md, "Fedora Copr"). Two halves:

  trigger   the copr-build job of .gitlab/ci/channels.yml. Dormant without the
            project's CI/CD variable COPR_WEBHOOK_URL (the Copr package's custom
            webhook, https://copr.fedorainfracloud.org/webhooks/custom/<id>/<uuid>/
            hmailserver/, protected and masked: its UUID is the secret, and unlike
            a Copr API token it does not expire). Otherwise: the release, as every
            channel takes it (build/ci/channels/release-files.py), which must be the
            feed's latest - the one srpm below builds, since Copr's builder has no
            HM_CHANNEL_VERSION - so a variable naming another is refused; its source
            tarball verified against its Sigstore bundle as this project's pipeline
            for the tag; the RPM spec inside it held to the release's version; and
            only then the webhook, which starts a build of the package's own source
            - this repository's master, by make_srpm. It waits for Copr to have made
            the source RPM, and says where the builds are.

  srpm      what Copr runs, from .copr/Makefile, in its own builder (a Fedora mock
            chroot, as root, with the network): the same release and the same
            verification - cosign out of the image .gitlab-ci.yml pins, the rule and
            the identity of release-files.py - and then the source RPM made from the
            verified tarball with the spec and Source1 taken out of that tarball,
            never out of this checkout (hmailserver/docs/SourcePackages.md). Copr
            then builds it in every chroot of the project. So what Copr builds is
            the signed release even though it cloned master.

Usage, from the root of a checkout:
  python3 build/ci/channels/copr-build.py trigger [--out DIR]
  python3 build/ci/channels/copr-build.py srpm --outdir DIR
  python3 build/ci/channels/copr-build.py --self-test
"""

import argparse
import importlib.util
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

# Where the tarball keeps the RPM spec and its Source1 (W2.2's source package).
RPM_DIR = "hmailserver/source/Server/platform/packaging/rpm"
SPEC = RPM_DIR + "/hmailserver.spec"
COPR = "https://copr.fedorainfracloud.org"
WEBHOOK = re.compile(r"^https://copr\.fedorainfracloud\.org/webhooks/custom/[0-9]+/[0-9a-f-]{36}/hmailserver/?$")


def say(line=""):
    print(line)
    sys.stdout.flush()


def spec_from_tarball(path, version, into):
    """The spec and every file it names as a local Source, out of the release's
    tarball, into `into`. Answers the spec's path; Refused when the tarball has none
    or its Version is not the release's."""
    top = "hmailserver-%s" % version
    with tarfile.open(path) as archive:
        names = dict((m.name, m) for m in archive.getmembers())
        member = names.get("%s/%s" % (top, SPEC))
        if member is None:
            raise Refused("v%s's tarball has no %s: the first release with the RPM spec is the one after W2.2 is folded"
                          % (version, SPEC))
        text = archive.extractfile(member).read().decode("utf-8")
        found = re.search(r"^Version:\s*(\S+)\s*$", text, re.MULTILINE)
        if not found or found.group(1) != version:
            raise Refused("v%s's spec says Version: %s" % (version, found.group(1) if found else "nothing"))
        os.makedirs(into, exist_ok=True)
        spec_path = os.path.join(into, "hmailserver.spec")
        with open(spec_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text.replace("\r\n", "\n"))
        for source in re.findall(r"^Source[1-9][0-9]*:\s*(\S+)\s*$", text, re.MULTILINE):
            if "/" in source:
                continue
            local = names.get("%s/%s/%s" % (top, RPM_DIR, source))
            if local is None:
                raise Refused("v%s's spec names %s, which its tarball does not carry beside it" % (version, source))
            with open(os.path.join(into, source), "wb") as f:
                f.write(archive.extractfile(local).read().replace(b"\r\n", b"\n"))
    return spec_path


# ---------------------------------------------------------------- srpm, in Copr's builder

def srpm(outdir, environ=None, run=subprocess.run):
    environ = os.environ if environ is None else environ
    fix = rf.fixture(environ)
    version, where = rf.release_version(environ, fix)
    say("the release: %s (from %s)" % (version, where))
    work = tempfile.mkdtemp(prefix="hm-copr-srpm-")
    try:
        tarball = "hmailserver-%s.tar.gz" % version
        cosign = rf.tool_path("cosign", work, environ)
        record = rf.verified_files(version, [tarball], os.path.join(work, "sources"), cosign, fix)
        sources = os.path.join(work, "sources")
        spec = spec_from_tarball(record[tarball]["path"], version, sources)
        os.makedirs(outdir, exist_ok=True)
        built = run(["rpmbuild", "-bs", "--define", "_sourcedir " + sources, "--define", "_srcrpmdir " + outdir,
                     "--define", "_topdir " + os.path.join(work, "rpmbuild"), spec], capture_output=True, text=True)
        if built.returncode != 0:
            raise Refused("rpmbuild -bs failed: %s" % (built.stderr or built.stdout).strip()[-600:])
        made = [n for n in os.listdir(outdir) if n.endswith(".src.rpm") and n.startswith("hmailserver-%s-" % version)]
        if len(made) != 1:
            raise Refused("rpmbuild -bs made %s in %s, not one source RPM of %s" % (made or "nothing", outdir, version))
        say("SOURCE RPM: %s, from v%s's tarball (SHA-256 %s), verified as %s"
            % (made[0], version, record[tarball]["sha256"], record[tarball]["identity"]))
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


# ---------------------------------------------------------------- trigger, in the pipeline

def trigger(out, environ=None, http=None, sleep=time.sleep):
    environ = os.environ if environ is None else environ
    http = http or rf.http
    fix = rf.fixture(environ)
    url = (environ.get("COPR_WEBHOOK_URL") or "").strip()
    if not url:
        say("not configured: COPR_WEBHOOK_URL is not set, so no build was started.")
        say("The owner makes the Copr project hmailserver and its package (make_srpm, from this repository's")
        say("master); a maintainer stores the package's custom webhook as the protected, masked CI/CD variable")
        say("COPR_WEBHOOK_URL (hmailserver/docs/Channels.md, 'Fedora Copr').")
        return 0
    if not WEBHOOK.match(url) and not (fix and fix.get("copr_webhook") == url):
        raise Refused("COPR_WEBHOOK_URL is not a Copr custom webhook of the package hmailserver"
                      " (https://copr.fedorainfracloud.org/webhooks/custom/<id>/<uuid>/hmailserver/)")

    version, where = rf.release_version(environ, fix)
    say("the release: %s (from %s)" % (version, where))
    # Copr's builder has no HM_CHANNEL_VERSION: srpm, there, takes the feed's latest.
    # So does this, or it would verify one release and start a build of another.
    latest, _ = rf.release_version(dict((k, v) for k, v in environ.items() if k != "HM_CHANNEL_VERSION"), fix)
    if latest != version:
        raise Refused("HM_CHANNEL_VERSION names %s, and Copr's builder builds the feed's latest, %s: Copr publishes the"
                      " latest release alone. Nothing was started." % (version, latest))
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    tarball = "hmailserver-%s.tar.gz" % version
    cosign = rf.tool_path("cosign", out, environ)
    record = rf.verified_files(version, [tarball], os.path.join(out, "release"), cosign, fix)
    rf.write_record(record, version, where, os.path.join(out, "verification.json"))
    spec_from_tarball(record[tarball]["path"], version, os.path.join(out, "spec"))
    say("v%s's tarball carries its RPM spec at that version" % version)

    status, _, body = http(url, data=b"", method="POST", headers={"Content-Type": "application/json"})
    if status not in (200, 201, 202):
        raise Refused("Copr's webhook answered HTTP %s: %s" % (status, body[:200].decode("utf-8", "replace")))
    build = re.search(rb"\b([0-9]{4,})\b", body or b"")
    if not build:
        say("PUBLISHED: Copr took the webhook (HTTP %s); its build list shows the build: %s/coprs/" % (status, COPR))
        return 0
    number = build.group(1).decode()
    say("Copr started build %s: %s/coprs/build/%s/" % (number, COPR, number))
    # The source RPM comes first; its failure is this job's to report. The chroots'
    # builds take an hour or more, and Copr shows them.
    state = None
    for attempt in range(40):
        status, answer = rf.http_json("%s/api_3/build/%s" % (COPR, number))
        state = (answer or {}).get("state") if status == 200 else None
        source_state = ((answer or {}).get("source_package") or {}).get("url")
        if state in ("failed", "canceled"):
            raise Refused("Copr build %s %s: %s/coprs/build/%s/" % (number, state, COPR, number))
        if state in ("running", "succeeded", "starting") and source_state:
            break
        sleep(30)
    say("PUBLISHED: Copr build %s of v%s is %s: %s/coprs/build/%s/" % (number, version, state or "queued", COPR, number))
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

    import io
    work = tempfile.mkdtemp(prefix="hm-copr-test-")
    try:
        def tarball(path, files):
            with tarfile.open(path, "w:gz") as archive:
                for name, data in files.items():
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
            return path
        spec = (b"Name: hmailserver\r\nVersion:        9.9.9\r\nSource0: https://gitlab.com/api/v4/projects/86730559/"
                b"packages/generic/hmailserver/%{version}/hmailserver-%{version}.tar.gz\r\nSource1:        hmailserver.sysusers\r\n")
        good = tarball(os.path.join(work, "good.tar.gz"), {"hmailserver-9.9.9/" + SPEC: spec,
                                                           "hmailserver-9.9.9/" + RPM_DIR + "/hmailserver.sysusers": b"u x\r\n"})
        path = spec_from_tarball(good, "9.9.9", os.path.join(work, "s"))
        with open(path, "rb") as f:
            expect("the spec comes out of the tarball, LF", b"\r" not in f.read())
        expect("  ... with its Source1 beside it", os.path.isfile(os.path.join(work, "s", "hmailserver.sysusers")))
        refused("a tarball without the spec (a release before W2.2)", lambda: spec_from_tarball(
            tarball(os.path.join(work, "none.tar.gz"), {"hmailserver-9.9.9/README": b"x"}), "9.9.9", os.path.join(work, "n")),
            "has no " + SPEC)
        refused("a spec of another version", lambda: spec_from_tarball(tarball(os.path.join(work, "old.tar.gz"), {
            "hmailserver-9.9.8/" + SPEC: spec}), "9.9.8", os.path.join(work, "o")), "Version: 9.9.9")
        refused("a spec whose Source1 the tarball lacks", lambda: spec_from_tarball(tarball(os.path.join(work, "s1.tar.gz"), {
            "hmailserver-9.9.9/" + SPEC: spec}), "9.9.9", os.path.join(work, "t")), "does not carry beside it")

        expect("the webhook's shape", WEBHOOK.match("https://copr.fedorainfracloud.org/webhooks/custom/123456/"
                                                    "0f0e8d6a-1b2c-4d5e-8f90-a1b2c3d4e5f6/hmailserver/") is not None)
        expect("  ... and another host refused", WEBHOOK.match("https://evil.example/webhooks/custom/1/"
                                                               "0f0e8d6a-1b2c-4d5e-8f90-a1b2c3d4e5f6/hmailserver/") is None)
        said = []
        held_say = globals()["say"]
        globals()["say"] = said.append
        try:
            expect("without COPR_WEBHOOK_URL: 'not configured', exit 0, nothing read",
                   trigger(os.path.join(work, "a"), {}, http=None) == 0 and said[0].startswith("not configured"))
        finally:
            globals()["say"] = held_say
        refused("a webhook on another host", lambda: trigger(os.path.join(work, "b"), {
            "COPR_WEBHOOK_URL": "https://evil.example/hook"}), "not a Copr custom webhook")
        posted = []
        held = (rf.release_version, rf.tool_path, rf.verified_files)
        try:
            rf.release_version = lambda environ, fix: ("9.9.9", "HM_CHANNEL_VERSION")
            rf.tool_path = lambda tool, work, environ=None: "cosign"

            def unverified(*args, **kwargs):
                raise Refused("hmailserver-9.9.9.tar.gz does not verify as signed by this project's pipeline for v9.9.9")
            rf.verified_files = unverified
            hook = "https://copr.fedorainfracloud.org/webhooks/custom/1/0f0e8d6a-1b2c-4d5e-8f90-a1b2c3d4e5f6/hmailserver/"
            globals()["say"] = lambda line="": None
            refused("with the webhook, a tarball that does not verify: refused before the webhook", lambda: trigger(
                os.path.join(work, "c"), {"COPR_WEBHOOK_URL": hook}, http=lambda *a, **k: posted.append(a)), "does not verify")
            expect("  ... the webhook was never called", posted == [])
            rf.verified_files = lambda version, names, into, cosign, fix=None, run=None: {names[0]: {
                "path": good, "sha256": "ab" * 32, "size": 1, "identity": rf.identities("9.9.9")[1],
                "repository_id": "86730559"}}
            code = trigger(os.path.join(work, "d"), {"COPR_WEBHOOK_URL": hook},
                           http=lambda *a, **k: posted.append((a, k)) or (200, {}, b""), sleep=lambda s: None)
            expect("with the webhook and a verified tarball carrying its spec: the webhook POSTed once, with nothing in it",
                   code == 0 and len(posted) == 1 and posted[0][0][0] == hook and posted[0][1].get("data") == b"")
            del posted[:]
            rf.release_version = lambda environ, fix: (
                ("9.9.8", "HM_CHANNEL_VERSION") if environ.get("HM_CHANNEL_VERSION") else ("9.9.9", "the feed"))
            refused("HM_CHANNEL_VERSION naming a release other than the feed's latest, which Copr's builder takes",
                    lambda: trigger(os.path.join(work, "e"), {"COPR_WEBHOOK_URL": hook, "HM_CHANNEL_VERSION": "9.9.8"},
                                    http=lambda *a, **k: posted.append(a)), "builds the feed's latest, 9.9.9")
            expect("  ... the webhook was never called", posted == [])
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
    commands = parser.add_subparsers(dest="command")
    t = commands.add_parser("trigger", help="the copr-build job: verify, then Copr's webhook")
    t.add_argument("--out", default="copr-build")
    s = commands.add_parser("srpm", help="Copr's make_srpm: the source RPM of the verified release")
    s.add_argument("--outdir", required=True)
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    try:
        if arguments.command == "trigger":
            return trigger(os.path.abspath(arguments.out))
        if arguments.command == "srpm":
            return srpm(os.path.abspath(arguments.outdir))
    except Refused as error:
        say("REFUSED: %s" % error)
        return 1
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
