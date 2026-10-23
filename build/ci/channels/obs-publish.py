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
     pipeline for the tag, the spec inside it held to the release's version, and
     every file the _service's extract_file steps take out of it found there (the
     spec, its Source1 and its rpmlint filters): a step that finds nothing fails
     the service on OBS, after this job has started it.
  4. GitLab's permanent link to the latest release's tarball - the address the
     _service fetches - downloaded and held to the verified SHA-256, so a release
     other than the latest (HM_CHANNEL_VERSION naming an older one) is refused, since
     the service always takes the latest. OBS downloads it again itself when the
     service runs and checks nothing but HTTPS: this holds the link to the verified
     bytes when the job looks, not when OBS does (hmailserver/docs/Channels.md).
  5. The OBS package's own _service, read from OBS's public API without the token,
     held to this tree's: OBS runs the copy the package holds, and the checks above
     are of this one. A package that holds another (the tree's changed, and the
     package not yet given the new one) is refused, with what to do.
  6. OBS's runservice trigger with the token, in a header, never on a command line.

Usage, from the root of a checkout:
  python3 build/ci/channels/obs-publish.py [--out DIR]
  python3 build/ci/channels/obs-publish.py --self-test
"""

import argparse
import fnmatch
import hashlib
import importlib.util
import os
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.parse
import xml.dom.minidom

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
FILTERS = "hmailserver/source/Server/platform/packaging/obs/hmailserver-rpmlintrc"
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


def extracted(root=ROOT):
    """What the _service's extract_file steps take out of the tarball, read from it:
    a list of (pattern, the name it is given or None). obs-service-extract_file hands
    each pattern to tar --wildcards, where * matches a slash too, as fnmatch's does."""
    with open(os.path.join(root, SERVICE), encoding="utf-8") as f:
        document = xml.dom.minidom.parseString(f.read())
    steps = []
    for service in document.getElementsByTagName("service"):
        if service.getAttribute("name") != "extract_file":
            continue
        params = {}
        for param in service.getElementsByTagName("param"):
            text = "".join(node.data for node in param.childNodes if node.nodeType == node.TEXT_NODE).strip()
            params.setdefault(param.getAttribute("name"), []).append(text)
        patterns = " ".join(params.get("files", [])).split()
        renamed = (params.get("outfilename") or [None])[0]
        if not patterns or (renamed and len(patterns) != 1):
            raise Refused("%s has an extract_file step that names %d files%s" % (
                SERVICE, len(patterns), " and an outfilename" if renamed else ""))
        steps.extend((pattern, renamed) for pattern in patterns)
    return steps


def tarball_carries(path, root=ROOT):
    """The tarball's members the _service extracts, one for each pattern; Refused when
    a pattern finds none, or a renamed one finds more than one."""
    with tarfile.open(path) as archive:
        names = [m.name for m in archive.getmembers() if m.isfile()]
    found = []
    for pattern, renamed in extracted(root):
        matches = [n for n in names if fnmatch.fnmatchcase(n, pattern)]
        if not matches:
            raise Refused("the tarball has no %s, which the _service extracts: the service would fail on OBS" % pattern)
        if renamed and len(matches) != 1:
            raise Refused("the tarball has %d files matching %s, which the _service extracts as %s"
                          % (len(matches), pattern, renamed))
        found.extend("%s%s" % (m.split("/")[-1], " (as %s)" % renamed if renamed else "") for m in matches)
    return found


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
    say("  ... and every file the _service extracts: %s" % ", ".join(tarball_carries(record[tarball]["path"], root)))

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
    holds = "%s/public/source/%s/%s/_service" % (api, urllib.parse.quote(project, safe=":"), PACKAGE)
    status, _, body = http(holds)
    with open(os.path.join(root, SERVICE), encoding="utf-8") as f:
        ours = f.read().replace("\r\n", "\n")
    if status != 200 or body.decode("utf-8", "replace").replace("\r\n", "\n") != ours:
        raise Refused("the OBS package %s/%s does not hold this tree's %s (%s answered HTTP %s%s): OBS runs the copy"
                      " the package holds, and this job checked the tree's. A maintainer puts the tree's file in the"
                      " package (osc, or the package's page), which runs the service; nothing was started."
                      % (project, PACKAGE, SERVICE.replace(os.sep, "/"), holds, status,
                         ", with another file" if status == 200 else ""))
    say("the OBS package %s/%s holds this tree's _service" % (project, PACKAGE))
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
    expect("  ... and is well-formed XML with its four services in order", [
        s.getAttribute("name") for s in xml.dom.minidom.parseString(service).getElementsByTagName("service")]
        == ["download_url", "extract_file", "extract_file", "download_files"])
    tree = "*/" + copr.RPM_DIR + "/"
    expect("  ... which extract the spec, its Source1 and the build's rpmlint filters, obs/hmailserver-rpmlintrc,"
           " under the name an OBS build's rpmlint loads from its sources", extracted() == [
               (tree + "hmailserver.spec", None), (tree + "hmailserver.sysusers", None), ("*/" + FILTERS, None)])
    expect("  ... each of them a file of this tree", all(
        os.path.isfile(os.path.join(ROOT, pattern[2:].replace("/", os.sep))) for pattern, _ in extracted()))

    def filters(relative):
        if not os.path.isfile(os.path.join(ROOT, relative.replace("/", os.sep))):
            return []
        with open(os.path.join(ROOT, relative.replace("/", os.sep)), encoding="utf-8") as f:
            return [line.strip() for line in f if line.startswith("addFilter(")]
    obs_filters, spec_filters = filters(FILTERS), filters(copr.RPM_DIR + "/hmailserver.rpmlintrc")
    expect("  ... whose filters are lines of rpm/hmailserver.rpmlintrc, the spec's own, and no others (%d of %d)"
           % (len(obs_filters), len(spec_filters)), bool(obs_filters) and all(line in spec_filters for line in obs_filters))

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

        def tarball(path, leave_out=None):
            with tarfile.open(path, "w:gz") as archive:
                for name, data in (("hmailserver-9.9.9/" + copr.SPEC, b"Version: 9.9.9\nSource1: hmailserver.sysusers\n"),
                                   ("hmailserver-9.9.9/" + copr.RPM_DIR + "/hmailserver.sysusers", b"u hmailserver -\n"),
                                   ("hmailserver-9.9.9/" + FILTERS, b"addFilter(\"x\")\n")):
                    if leave_out and name.endswith(leave_out):
                        continue
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
            with open(path, "rb") as f:
                return f.read()
        good = os.path.join(work, "good.tar.gz")
        good_bytes = tarball(good)
        unfiltered = os.path.join(work, "unfiltered.tar.gz")
        unfiltered_bytes = tarball(unfiltered, "/hmailserver-rpmlintrc")
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
            def verified_as(path, data):
                return lambda version, names, into, cosign, fix=None, run=None: {names[0]: {
                    "path": path, "sha256": hashlib.sha256(data).hexdigest(), "size": 1,
                    "identity": rf.identities("9.9.9")[1], "repository_id": "86730559"}}

            def world(latest, holds=service.encode("utf-8")):
                def http(url, data=None, headers=None, method=None, timeout=120, fix=None):
                    calls.append((url, method, headers))
                    if "permalink" in url:
                        return 200, {}, latest
                    if "/public/source/" in url:
                        return (200, {}, holds) if holds is not None else (404, {}, b'<status code="unknown_package"/>')
                    return 200, {}, b'<status code="ok">\n  <summary>Ok</summary>\n</status>\n'
                return http
            rf.verified_files = verified_as(unfiltered, unfiltered_bytes)
            del calls[:]
            refused("a verified tarball without the rpmlint filters the _service extracts: refused before OBS is called",
                    lambda: obs_publish(os.path.join(work, "f"), environ, http=world(unfiltered_bytes)),
                    "has no */" + FILTERS)
            expect("  ... OBS was never called", calls == [])
            rf.verified_files = verified_as(good, good_bytes)
            del calls[:]
            refused("the permanent link leading to another release's tarball: refused before OBS is called",
                    lambda: obs_publish(os.path.join(work, "d"), environ, http=world(b"another")), "is not v9.9.9's")
            expect("  ... OBS was never called", not [c for c in calls if "trigger" in c[0]])
            earlier = re.sub(r'\s*<service name="extract_file">\s*<param name="archive">[^<]*</param>\s*'
                             r'<param name="files">\*/' + re.escape(FILTERS) + r'</param>\s*</service>', "", service)
            for what, holds in (("the earlier _service, without the filters' step", earlier.encode("utf-8")),
                                ("no _service at all", None)):
                del calls[:]
                refused("the OBS package holding %s: refused before the trigger" % what,
                        lambda: obs_publish(os.path.join(work, "g"), environ, http=world(good_bytes, holds)),
                        "does not hold this tree's")
                expect("  ... the trigger was never called", not [c for c in calls if "trigger" in c[0]]
                       and earlier != service)
            del calls[:]
            code = obs_publish(os.path.join(work, "e"), environ, http=world(good_bytes))
            triggers = [c for c in calls if "/trigger/runservice" in c[0]]
            reads = [c for c in calls if "/public/source/" in c[0]]
            expect("verified, the link held to it, the package's _service read without the token and held to the"
                   " tree's, then runservice with the token in a header",
                   code == 0 and len(triggers) == 1 and triggers[0][1] == "POST"
                   and [c[0] for c in reads] == [OBS_API + "/public/source/home:hmailserver/hmailserver/_service"]
                   and not reads[0][2]
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
