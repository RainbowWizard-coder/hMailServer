#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The obs-publish job of .gitlab/ci/channels.yml: the release built by the openSUSE
Build Service for Leap and Tumbleweed, whose signed repository zypper takes
(hmailserver/docs/Channels.md, "openSUSE Build Service").

The OBS package holds one file, hmailserver/source/Server/platform/packaging/obs/
_service, which fetches the latest release's source tarball through GitLab's
permanent link and builds it with the spec inside it. This job decides when that
runs, and runs it only for a verified release:

  1. Dormant: without the project's CI/CD variables OBS_RUNSERVICE_TOKEN (an OBS
     token of the operation runservice for the package: osc token --create
     --operation runservice <project> hmailserver; protected, masked; it does not
     expire) and OBS_PROJECT (the OBS project that holds the package, such as
     home:<owner>) it says "not configured" and exits 0.
  2. The release, as every channel takes it (build/ci/channels/release-files.py).
  3. Its source tarball verified against its Sigstore bundle as this project's
     pipeline for the tag, and the spec inside it held to the release's version.
  4. GitLab's permanent link to the latest release's tarball - the address the
     _service fetches - downloaded and held to the verified SHA-256, so a release
     other than the latest (HM_CHANNEL_VERSION naming an older one) is refused, since
     the service always takes the latest. OBS downloads it again itself when the
     service runs and checks nothing but HTTPS: this holds the link to the verified
     bytes when the job looks, not when OBS does (hmailserver/docs/Channels.md).
  5. OBS's runservice trigger with the token, in a header, never on a command line.

Usage, from the root of a checkout:
  python3 build/ci/channels/obs-publish.py [--out DIR]
  python3 build/ci/channels/obs-publish.py --self-test
"""

import argparse
import hashlib
import importlib.util
import os
import re
import shutil
import sys
import tempfile
import urllib.parse

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
copr = load("copr_build", os.path.join(HERE, "copr-build.py"))
Refused = rf.Refused

SERVICE = os.path.join("hmailserver", "source", "Server", "platform", "packaging", "obs", "_service")
OBS_API = "https://api.opensuse.org"
PACKAGE = "hmailserver"
PROJECT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.+-]*(:[A-Za-z0-9_.+-]+)*$")


def say(line=""):
    print(line)
    sys.stdout.flush()


def permalink(root=ROOT):
    """The address the _service fetches, read from it."""
    with open(os.path.join(root, SERVICE), encoding="utf-8") as f:
        text = f.read()
    found = re.findall(r'<param name="url">([^<]+)</param>', text)
    if len(found) != 1 or not found[0].startswith("https://gitlab.com/"):
        raise Refused("%s does not fetch one GitLab address" % SERVICE)
    return found[0]


def obs_publish(out, environ=None, http=None, root=ROOT):
    environ = os.environ if environ is None else environ
    http = http or rf.http
    fix = rf.fixture(environ)
    token = environ.get("OBS_RUNSERVICE_TOKEN")
    project = (environ.get("OBS_PROJECT") or "").strip()
    if not token:
        say("not configured: OBS_RUNSERVICE_TOKEN is not set, so the build service was not started.")
        say("The owner makes the OBS project and its package hmailserver with the tree's _service in it; a")
        say("maintainer stores a runservice token for the package as the protected, masked CI/CD variable")
        say("OBS_RUNSERVICE_TOKEN, and the project's name as OBS_PROJECT (hmailserver/docs/Channels.md).")
        return 0
    if not PROJECT.match(project):
        raise Refused("OBS_PROJECT is %r: the OBS project that holds the package hmailserver (home:<owner>)" % project)

    version, where = rf.release_version(environ, fix)
    say("the release: %s (from %s)" % (version, where))
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    tarball = "hmailserver-%s.tar.gz" % version
    cosign = rf.tool_path("cosign", out, environ)
    record = rf.verified_files(version, [tarball], os.path.join(out, "release"), cosign, fix)
    rf.write_record(record, version, where, os.path.join(out, "verification.json"))
    copr.spec_from_tarball(record[tarball]["path"], version, os.path.join(out, "spec"))
    say("v%s's tarball carries its RPM spec at that version" % version)

    link = (fix.get("obs_permalink") if fix else None) or permalink(root)
    status, _, body = http(link, timeout=600)
    if status != 200:
        raise Refused("%s answered HTTP %s: the latest release has no tarball there" % (link, status))
    fetched = hashlib.sha256(body).hexdigest()
    if fetched != record[tarball]["sha256"]:
        raise Refused("%s is not v%s's verified tarball (SHA-256 %s, not %s): the service takes the latest release,"
                      " and that is not this one. Nothing was started." % (link, version, fetched, record[tarball]["sha256"]))
    say("%s is v%s's verified tarball, byte for byte" % (link, version))

    api = (fix.get("obs_api") if fix else None) or OBS_API
    trigger = "%s/trigger/runservice?%s" % (api, urllib.parse.urlencode([("project", project), ("package", PACKAGE)]))
    status, _, body = http(trigger, data=b"", method="POST", headers={"Authorization": "Token " + token})
    token = None
    answer = body.decode("utf-8", "replace")
    if status != 200 or 'code="ok"' not in answer:
        raise Refused("OBS refused the runservice trigger (HTTP %s): %s" % (status, re.sub(r"\s+", " ", answer)[:300]))
    say("PUBLISHED: OBS runs the service of %s/%s for v%s: https://build.opensuse.org/package/show/%s/%s"
        % (project, PACKAGE, version, project, PACKAGE))
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

    expect("the _service fetches the latest release's tarball through GitLab's permanent link", permalink() ==
           "https://gitlab.com/hmailserver/hmailserver/-/releases/permalink/latest/downloads/hmailserver.tar.gz")
    with open(os.path.join(ROOT, SERVICE), encoding="utf-8") as f:
        service = f.read()
    expect("  ... and names no version, and no token", not re.search(r"\d+\.\d+\.\d+", service) and "token" not in service.lower()
           .replace("no token", ""))
    import xml.dom.minidom
    expect("  ... and is well-formed XML with its three services in order", [
        s.getAttribute("name") for s in xml.dom.minidom.parseString(service).getElementsByTagName("service")]
        == ["download_url", "extract_file", "download_files"])

    work = tempfile.mkdtemp(prefix="hm-obs-test-")
    said = []
    held_say = globals()["say"]
    try:
        globals()["say"] = said.append
        expect("without OBS_RUNSERVICE_TOKEN: 'not configured', exit 0, nothing read",
               obs_publish(os.path.join(work, "a"), {}, http=None) == 0 and said[0].startswith("not configured"))
        globals()["say"] = lambda line="": None
        refused("a token without OBS_PROJECT", lambda: obs_publish(os.path.join(work, "b"), {"OBS_RUNSERVICE_TOKEN": "t"}),
                "OBS_PROJECT is ''")

        import io
        import tarfile
        good = os.path.join(work, "good.tar.gz")
        with tarfile.open(good, "w:gz") as archive:
            for name, data in (("hmailserver-9.9.9/" + copr.SPEC, b"Version: 9.9.9\nSource1: x\n"),
                               ("hmailserver-9.9.9/" + copr.RPM_DIR + "/x", b"x")):
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
        with open(good, "rb") as f:
            good_bytes = f.read()
        held = (rf.release_version, rf.tool_path, rf.verified_files)
        calls = []
        try:
            rf.release_version = lambda environ, fix: ("9.9.9", "HM_CHANNEL_VERSION")
            rf.tool_path = lambda tool, work, environ=None: "cosign"

            def unverified(*args, **kwargs):
                raise Refused("hmailserver-9.9.9.tar.gz does not verify as signed by this project's pipeline for v9.9.9")
            rf.verified_files = unverified
            environ = {"OBS_RUNSERVICE_TOKEN": "obs-secret-token", "OBS_PROJECT": "home:hmailserver"}
            refused("with the token, a tarball that does not verify: refused before OBS is called", lambda: obs_publish(
                os.path.join(work, "c"), environ, http=lambda *a, **k: calls.append(a)), "does not verify")
            expect("  ... OBS was never called", calls == [])
            rf.verified_files = lambda version, names, into, cosign, fix=None, run=None: {names[0]: {
                "path": good, "sha256": hashlib.sha256(good_bytes).hexdigest(), "size": 1,
                "identity": rf.identities("9.9.9")[1], "repository_id": "86730559"}}

            def world(latest):
                def http(url, data=None, headers=None, method=None, timeout=120, fix=None):
                    calls.append((url, method, headers))
                    if "permalink" in url:
                        return 200, {}, latest
                    return 200, {}, b'<status code="ok">\n  <summary>Ok</summary>\n</status>\n'
                return http
            del calls[:]
            refused("the permanent link leading to another release's tarball: refused before OBS is called",
                    lambda: obs_publish(os.path.join(work, "d"), environ, http=world(b"another")), "is not v9.9.9's")
            expect("  ... OBS was never called", not [c for c in calls if "trigger" in c[0]])
            del calls[:]
            code = obs_publish(os.path.join(work, "e"), environ, http=world(good_bytes))
            triggers = [c for c in calls if "/trigger/runservice" in c[0]]
            expect("verified, the link held to it, then runservice with the token in a header",
                   code == 0 and len(triggers) == 1 and triggers[0][1] == "POST"
                   and triggers[0][0] == OBS_API + "/trigger/runservice?project=home%3Ahmailserver&package=hmailserver"
                   and triggers[0][2] == {"Authorization": "Token obs-secret-token"})
        finally:
            rf.release_version, rf.tool_path, rf.verified_files = held
    finally:
        globals()["say"] = held_say
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
    parser.add_argument("--out", default="obs-publish")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    try:
        return obs_publish(os.path.abspath(arguments.out))
    except Refused as error:
        say("REFUSED: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
