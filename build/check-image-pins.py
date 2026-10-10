#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every image the CI files and the build scripts name by digest, still in its registry.

An image pinned by digest (name[:tag]@sha256:<digest>) runs those bytes or nothing: a
registry that no longer serves the digest fails the job at its pull, whatever the job
was for. GitLab's registry stops serving a digest of its analyzers within days of the
tag moving on: semgrep-sast's of 26 September 2026 answered "manifest unknown" on 5
October and held a release tag's pipeline, secret_detection's of 5 October failed
master's pipelines of 7 and 9 October before it ran, and container_scanning's answered
404 on 10 October - a job that runs for a release tag alone, so nothing had shown it.
This finds such a pin before a job needs it: each reference is asked of its registry
as a runner asks it, without credentials - a HEAD of /v2/<repository>/manifests/
<digest>, with the anonymous bearer token the registry's challenge names - and a
registry that says it does not have the digest fails the check, naming the pin and
every place that names it. A pin is only the bytes it names, so this asks nothing
else: not whether a newer image exists (Renovate's work), and not whether the bytes
are safe (the scans').

What is read: .gitlab-ci.yml and the files under .gitlab/ci/, every file under build/
(the CI scripts, the lab, the fixtures' builders), and every Dockerfile, *.Dockerfile,
Containerfile and Compose file of the tree - the tracked files, as git ls-files lists
them, or every file there when git is not to be had (a job's image without it). Not
the documents, which record digests as they were, and not the help pages made from
them. A reference is a name, an optional tag and @sha256: with 64 hex digits, wherever
it stands - an image: line, a FROM, a shell default (${VAR:-name@sha256:...}), a
string in a script.

It runs in the pipeline of Renovate's schedule (.gitlab-ci.yml's image-pins job), daily,
and by hand before a release tag (RELEASE.md). Exit codes: 0 every pin resolves; 1 a
registry does not have a pin's digest (404, or a refusal - 401 or 403 - with its own
anonymous token, which a repository that is gone or private gives), or answers it with
another digest; 2 a registry could not be asked - the network, a 429 or a 5xx, three
times - and nothing was said to be gone, or the check was asked wrongly. Once a registry
could not be asked, the pins after it on that registry are not asked, and none is asked
after the check's deadline (DEADLINE, ten minutes, below the job's timeout): each such
pin is named, with the places that name it, as one that could not be asked. Docker Hub
does not count a HEAD against its pull limit.

  python build/check-image-pins.py             ask every registry
  python build/check-image-pins.py --list      the pins and where each is named, asking nothing
  python build/check-image-pins.py --self-test against a registry it serves on the loopback
"""

import os
import sys

# Nothing is imported by name from this script's own directory, which Python puts first
# on sys.path (build/check-release-chain.py, PATH_GUARD).
sys.path[:] = [p for p in sys.path if os.path.normcase(os.path.realpath(p or os.curdir)) !=
               os.path.normcase(os.path.dirname(os.path.realpath(__file__)))]

import argparse
import http.server
import json
import re
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# A reference: [registry[:port]/]path[:tag]@sha256:<64 hex>. Not after a character a
# name could have gone on with (so that no tail of a longer name is taken for one), but
# after the - of a shell default, ${VAR:-name@...}.
REFERENCE = re.compile(r"(?<![A-Za-z0-9._/@:])((?:[a-z0-9][a-z0-9.-]*(?::[0-9]+)?/)?[a-z0-9][a-z0-9._-]*(?:/[a-z0-9][a-z0-9._-]*)*)"
                       r"(?::([A-Za-z0-9_][A-Za-z0-9_.-]{0,127}))?@(sha256:[0-9a-f]{64})(?![0-9a-f])")
SCANNED_DIRECTORIES = (".gitlab/ci/", "build/")
SCANNED_FILES = (".gitlab-ci.yml",)
CONTAINER_FILE = re.compile(r"(?:^|/)(?:Dockerfile[^/]*|[^/]*\.Dockerfile|Containerfile[^/]*|(?:docker-)?compose[^/]*\.ya?ml)$")
ACCEPT = ", ".join(["application/vnd.oci.image.index.v1+json", "application/vnd.oci.image.manifest.v1+json",
                    "application/vnd.docker.distribution.manifest.list.v2+json",
                    "application/vnd.docker.distribution.manifest.v2+json"])
TRIES = 3
WAIT = (5, 20)
TIMEOUT = 30
# How long the check asks at most, in seconds: below the image-pins job's timeout (15
# minutes), so that a registry that does not answer ends the check with exit 2 and the
# pins it could not ask named, not the job with GitLab's job_execution_timeout. A pin
# is not asked once the deadline has passed, nor asked again once the next try would
# begin after it; one try - a HEAD, the token, a HEAD - can still take 3 x TIMEOUT.
DEADLINE = 600
# The registries' own hosts, for the self-test to point at the loopback.
ENDPOINT = {}


def in_scope(path):
    path = path.replace("\\", "/")
    return path in SCANNED_FILES or path.startswith(SCANNED_DIRECTORIES) or CONTAINER_FILE.search(path) is not None


def tree_files(root):
    """The tree's files in scope, relative to root: git's list when there is git, else a walk."""
    try:
        done = subprocess.run(["git", "-C", root, "ls-files", "-z"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if done.returncode == 0 and done.stdout:
            return sorted(p for p in done.stdout.decode("utf-8", "replace").split("\0") if p and in_scope(p))
    except OSError:
        pass
    found = []
    for directory, subdirectories, files in os.walk(root):
        subdirectories[:] = [d for d in subdirectories if d != ".git"]
        for name in files:
            relative = os.path.relpath(os.path.join(directory, name), root).replace("\\", "/")
            if in_scope(relative):
                found.append(relative)
    return sorted(found)


def pins_in(text):
    """[(reference, line)] of a text, the reference as written: name[:tag]@sha256:<digest>."""
    found = []
    for number, line in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
        for match in REFERENCE.finditer(line):
            name, tag, digest = match.groups()
            found.append(("%s%s@%s" % (name, ":" + tag if tag else "", digest), number))
    return found


def tree_pins(root):
    """{reference: [path:line, ...]} of every pin the tree's files in scope name."""
    pins = {}
    for path in tree_files(root):
        try:
            with open(os.path.join(root, path), "rb") as f:
                data = f.read()
        except OSError:
            continue
        if b"\0" in data[:8192]:
            continue
        for reference, line in pins_in(data.decode("utf-8", "replace")):
            pins.setdefault(reference, []).append("%s:%d" % (path, line))
    return pins


def locate(reference):
    """(registry host, repository, digest) of a reference, as docker resolves the name."""
    name, digest = reference.split("@", 1)
    first, _, rest = name.partition("/")
    if rest and ("." in first or ":" in first or first == "localhost"):
        host, repository = first, rest
    else:
        host, repository = "docker.io", name
    repository = re.sub(r":[A-Za-z0-9_][A-Za-z0-9_.-]*$", "", repository)
    if host in ("docker.io", "index.docker.io", "registry-1.docker.io"):
        host = "registry-1.docker.io"
        if "/" not in repository:
            repository = "library/" + repository
    return host, repository, digest


def endpoint(host):
    return ENDPOINT.get(host, "https://" + host)


class Transient(Exception):
    pass


def head(url, token=None):
    """(status, headers) of a HEAD; a status the registry gave is returned, not raised."""
    request = urllib.request.Request(url, method="HEAD", headers={"Accept": ACCEPT, "User-Agent": "hmailserver-check-image-pins"})
    if token:
        request.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT) as answer:
            return answer.status, answer.headers
    except urllib.error.HTTPError as e:
        return e.code, e.headers
    except (urllib.error.URLError, OSError) as e:
        raise Transient("%s: %s" % (type(e).__name__, getattr(e, "reason", e)))


def anonymous_token(challenge, repository):
    """The token a Bearer challenge's realm gives anyone for pulling repository, or None."""
    if not challenge or not challenge.strip().lower().startswith("bearer"):
        return None
    fields = dict(re.findall(r'(\w+)="([^"]*)"', challenge))
    realm = fields.get("realm")
    if not realm:
        return None
    query = {"scope": "repository:%s:pull" % repository}
    if fields.get("service"):
        query["service"] = fields["service"]
    url = realm + ("&" if "?" in realm else "?") + urllib.parse.urlencode(query)
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "hmailserver-check-image-pins"}),
                                    timeout=TIMEOUT) as answer:
            body = json.loads(answer.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code in (429,) or e.code >= 500:
            raise Transient("the token service answered %d" % e.code)
        return None
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise Transient("the token service: %s" % e)
    return body.get("token") or body.get("access_token")


def ask_once(reference):
    """("ok"|"gone", what) for one reference, or Transient."""
    host, repository, digest = locate(reference)
    url = "%s/v2/%s/manifests/%s" % (endpoint(host), repository, digest)
    status, headers = head(url)
    if status == 401:
        token = anonymous_token(headers.get("WWW-Authenticate", ""), repository)
        if token:
            status, headers = head(url, token)
    if status == 429 or status >= 500:
        raise Transient("%s answered %d" % (host, status))
    if status == 200:
        served = (headers.get("Docker-Content-Digest") or "").strip()
        if served and served != digest:
            return "gone", "%s answers the digest with another, %s" % (host, served)
        return "ok", "200"
    if status in (401, 403, 404):
        return "gone", "%s answered %d%s" % (host, status, " (it has no such manifest)" if status == 404 else
                                             " to its own anonymous token (the repository is gone, or not public)")
    raise Transient("%s answered %d" % (host, status))


def ask(reference, sleep=time.sleep, give_up_at=None, clock=time.monotonic):
    """("ok"|"gone"|"unknown", what), asking up to TRIES times when the answer is no answer,
    and not again when the next try would begin after give_up_at (clock's time)."""
    why = ""
    for attempt in range(TRIES):
        try:
            return ask_once(reference)
        except Transient as e:
            why = str(e)
            if attempt + 1 < TRIES:
                wait = WAIT[min(attempt, len(WAIT) - 1)]
                if give_up_at is not None and clock() + wait >= give_up_at:
                    return "unknown", "%s (asked %d time(s); the check's deadline came before the next)" % (why, attempt + 1)
                sleep(wait)
    return "unknown", "%s (asked %d times)" % (why, TRIES)


def check(pins, say=print, sleep=time.sleep, clock=time.monotonic, deadline=DEADLINE):
    """Ask every pin, within deadline seconds; say what each answered; the exit code. Once
    a registry could not be asked, the pins after it on the same registry are not asked,
    and none is asked once the deadline has passed: each is then "could not be asked"."""
    gone, unknown = [], []
    give_up_at = clock() + deadline
    unreachable = {}
    for reference in sorted(pins):
        host = locate(reference)[0]
        if host in unreachable:
            state, what = "unknown", "not asked: %s could not be asked for %s" % (host, unreachable[host])
        elif clock() >= give_up_at:
            state, what = "unknown", "not asked: the check's %d-second deadline had passed" % deadline
        else:
            state, what = ask(reference, sleep, give_up_at, clock)
            if state == "unknown":
                unreachable[host] = reference
        if state == "ok":
            say("ok       %s" % reference)
            continue
        say("%-8s %s: %s" % ("GONE" if state == "gone" else "UNKNOWN", reference, what))
        for place in pins[reference]:
            say("           named at %s" % place)
        (gone if state == "gone" else unknown).append(reference)
    say("")
    say("%d pin(s) in %d place(s): %d resolve, %d gone, %d could not be asked"
        % (len(pins), sum(len(p) for p in pins.values()), len(pins) - len(gone) - len(unknown), len(gone), len(unknown)))
    if gone:
        say("Gone from its registry - a job that pulls it fails at the pull:")
        for reference in gone:
            say("  %s (%s)" % (reference, ", ".join(pins[reference])))
        say("Re-take the digest, or the project's copy of the image, in each place named (ContinuousIntegration.md, "
            "\"Image pins\").")
        if unknown:
            say("And %d pin(s) could not be asked, which a run after this one asks again." % len(unknown))
        return 1
    if unknown:
        say("Not every registry could be asked; nothing was said to be gone. Not asked, or asked without an answer:")
        for reference in unknown:
            say("  %s (%s)" % (reference, ", ".join(pins[reference])))
        return 2
    return 0


# ---------------------------------------------------------------- the self-test

class FakeRegistry(http.server.BaseHTTPRequestHandler):
    """A registry on the loopback: manifests by digest behind a bearer challenge."""
    served = {}       # repository -> {digest: digest it answers with}
    flaky = {}        # repository -> how many 503s before it answers
    always = set()    # repositories that answer 503 for ever
    asked = []

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path.startswith("/token?"):
            query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            body = json.dumps({"token": "t-" + query.get("scope", [""])[0]}).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(405)
        self.end_headers()

    def do_HEAD(self):
        match = re.match(r"^/v2/(.+)/manifests/(sha256:[0-9a-f]{64})$", self.path)
        FakeRegistry.asked.append(self.path)
        if not match:
            self.send_response(400)
            self.end_headers()
            return
        repository, digest = match.groups()
        authorization = self.headers.get("Authorization", "")
        if authorization != "Bearer t-repository:%s:pull" % repository:
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Bearer realm="http://%s/token",service="fake"' % self.headers.get("Host"))
            self.end_headers()
            return
        if repository in FakeRegistry.always:
            self.send_response(503)
            self.end_headers()
            return
        if FakeRegistry.flaky.get(repository, 0) > 0:
            FakeRegistry.flaky[repository] -= 1
            self.send_response(503)
            self.end_headers()
            return
        answer = FakeRegistry.served.get(repository, {}).get(digest)
        if answer is None:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Docker-Content-Digest", answer)
        self.send_header("Content-Type", "application/vnd.oci.image.index.v1+json")
        self.end_headers()


def self_test(root=ROOT):
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    one, two, three = "sha256:" + "1" * 64, "sha256:" + "2" * 64, "sha256:" + "3" * 64
    text = ("job:\n  image: python:3.14-slim-trixie@%s\n"
            "  services:\n    - name: postgres:18@%s\n"
            "  image:\n    name: registry.gitlab.com/hmailserver/hmailserver/mirror/secrets:7.40.0-111111111111@%s\r\n"
            "FROM --platform=$BUILDPLATFORM docker.io/library/ubuntu:26.04@%s AS build\n"
            'image="${HM_IMAGE:-registry.fedoraproject.org/fedora:44@%s}"\n'
            "    'alpine@%s',\n"
            "  image: localhost:5000/a/b:c@%s\n"
            "none: postgres:18, sha256:%s alone, x@sha256:%s (63 digits)\n"
            "and a letter after the digest: registry.example/x@sha256:%sZ\n"
            % (one, two, one, three, two, three, one, "4" * 64, "5" * 63, "6" * 64))
    found = pins_in(text)
    expect([r for r, _ in found] == ["python:3.14-slim-trixie@" + one, "postgres:18@" + two,
                                     "registry.gitlab.com/hmailserver/hmailserver/mirror/secrets:7.40.0-111111111111@" + one,
                                     "docker.io/library/ubuntu:26.04@" + three, "registry.fedoraproject.org/fedora:44@" + two,
                                     "alpine@" + three, "localhost:5000/a/b:c@" + one, "registry.example/x@sha256:" + "6" * 64],
           "the references of a text, as written: %s" % found)
    expect([n for _, n in found] == [2, 4, 6, 7, 8, 9, 10, 12], "each at its line, CRLF or LF: %s" % found)
    # A digest followed by more hex is no reference (65 digits); one followed by a letter is.
    expect(not pins_in("x@sha256:%s" % ("7" * 65)), "65 hex digits are no digest")

    expect(locate("alpine:3.24@" + one) == ("registry-1.docker.io", "library/alpine", one), "a Docker Hub library image")
    expect(locate("renovate/renovate:44@" + one) == ("registry-1.docker.io", "renovate/renovate", one), "a Docker Hub user's image")
    expect(locate("docker.io/library/ubuntu:26.04@" + one) == ("registry-1.docker.io", "library/ubuntu", one), "docker.io named")
    expect(locate("docker.io/regclient/regctl:v0.11.6@" + one) == ("registry-1.docker.io", "regclient/regctl", one), "docker.io/user")
    expect(locate("mcr.microsoft.com/dotnet/sdk:10.0@" + one) == ("mcr.microsoft.com", "dotnet/sdk", one), "another registry")
    expect(locate("localhost:5000/a/b:c@" + one) == ("localhost:5000", "a/b", one), "a registry with a port")
    expect(locate("registry.gitlab.com/hmailserver/hmailserver/ci-linux:26.04-52e9976e73bf@" + one)
           == ("registry.gitlab.com", "hmailserver/hmailserver/ci-linux", one), "a tag with a hyphen")

    expect(in_scope(".gitlab-ci.yml") and in_scope(".gitlab/ci/packages-rpm.yml") and in_scope("build/ci/linux.Dockerfile")
           and in_scope("build/lab/lab.sh") and in_scope("hmailserver/test/Docker/Dockerfile")
           and in_scope("hmailserver/source/Server/platform/packaging/docker/docker-compose.yml"), "the CI files and build scripts")
    expect(not in_scope("hmailserver/docs/ContinuousIntegration.md") and not in_scope("CHANGELOG.md")
           and not in_scope("hmailserver/source/Server/Common/Util/HelpPagesData.cpp") and not in_scope(".gitlab/CODEOWNERS"),
           "not the documents, nor the help pages made from them")

    # The tree's own pins: the CI files and the scripts under build/ are read.
    own = tree_pins(root)
    where = sorted(set(p.rsplit(":", 1)[0] for places in own.values() for p in places))
    expect(len(own) >= 40, "the tree names some forty images by digest or more: %d" % len(own))
    for path in (".gitlab-ci.yml", ".gitlab/ci/gitlab-security.yml", ".gitlab/ci/container-image.yml", "build/ci/linux.Dockerfile",
                 "build/lab/docker-compose.yml", "build/ci/container-image-start-test.sh"):
        expect(path in where, "%s's pins are read: %s" % (path, where))
    expect(not any(p.startswith("hmailserver/docs/") for p in where), "no document's pins are read")
    expect(any("/mirror/secrets:" in r for r in own), "secret_detection's image is among them")

    # The registry's side: on the loopback, behind a bearer challenge.
    FakeRegistry.served = {"library/alpine": {one: one}, "a/b": {two: three}, "c/d": {one: one}}
    FakeRegistry.flaky = {"c/d": 1}
    FakeRegistry.always = {"e/f"}
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeRegistry)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    held = dict(ENDPOINT)
    try:
        address = "http://127.0.0.1:%d" % server.server_address[1]
        for host in ("registry-1.docker.io", "registry.example"):
            ENDPOINT[host] = address
        slept = []
        nap = slept.append
        expect(ask("alpine:3.24@" + one, nap) == ("ok", "200"), "a digest the registry has, through its token")
        state, what = ask("alpine:3.24@" + two, nap)
        expect(state == "gone" and "404" in what, "a digest it has not: gone, 404 (%s %s)" % (state, what))
        state, what = ask("registry.example/a/b@" + two, nap)
        expect(state == "gone" and "another" in what, "a digest answered with another: gone (%s %s)" % (state, what))
        del slept[:]
        expect(ask("registry.example/c/d@" + one, nap) == ("ok", "200") and slept == [5], "a 503 asked again: %s" % slept)
        del slept[:]
        state, what = ask("registry.example/e/f@" + one, nap)
        expect(state == "unknown" and slept == [5, 20] and "asked 3 times" in what, "503 three times: unknown (%s %s %s)"
               % (state, what, slept))

        said = []
        code = check({"alpine:3.24@" + one: ["a.yml:1"], "alpine:3.24@" + two: ["b.yml:2", "build/c.sh:3"]}, said.append, nap)
        text = "\n".join(said)
        expect(code == 1 and "GONE     alpine:3.24@" + two in text and "named at b.yml:2" in text and "named at build/c.sh:3" in text,
               "a gone pin fails the check, named with every place: %s\n%s" % (code, text))
        said = []
        code = check({"alpine:3.24@" + one: ["a.yml:1"], "registry.example/e/f@" + one: ["d.yml:4"]}, said.append, nap)
        expect(code == 2 and "UNKNOWN" in "\n".join(said), "a registry that could not be asked: exit 2: %s %s" % (code, said))
        said = []
        code = check({"alpine:3.24@" + one: ["a.yml:1"], "registry.example/e/f@" + one: ["d.yml:4"],
                      "alpine@" + three: ["e.yml:5"]}, said.append, nap)
        expect(code == 1, "gone outweighs could-not-ask: %s" % code)
        said = []
        expect(check({"alpine:3.24@" + one: ["a.yml:1"]}, said.append, nap) == 0, "every pin resolves: exit 0 (%s)" % said)
        # A registry that could not be asked is not asked again for the pins after it, which
        # are named as not asked; another registry still is. e/g would answer 404 - gone,
        # exit 1 - if it were asked.
        del FakeRegistry.asked[:]
        said = []
        code = check({"alpine:3.24@" + one: ["a.yml:1"], "registry.example/e/f@" + one: ["d.yml:4"],
                      "registry.example/e/g@" + one: ["f.yml:6"]}, said.append, nap)
        text = "\n".join(said)
        expect(code == 2 and "ok       alpine:3.24@" + one in text and not any("/e/g/" in p for p in FakeRegistry.asked)
               and "not asked: registry.example could not be asked for registry.example/e/f@" + one in text
               and "  registry.example/e/g@%s (f.yml:6)" % one in text,
               "the pins after an unanswering registry's first are not asked, and named: %s\n%s" % (code, text))
        # The deadline: no try begins after it - here the next would, so the pin is given up
        # after one sleep - and no pin is asked once it has passed. a/b would answer with
        # another digest - gone, exit 1 - if it were asked.
        now = [0]
        slept = []
        state, what = ask("registry.example/e/f@" + one, lambda s: (slept.append(s), now.__setitem__(0, now[0] + s)), 10,
                          lambda: now[0])
        expect(state == "unknown" and slept == [5] and "deadline" in what, "no try begins after the deadline: %s %s %s"
               % (state, what, slept))
        del FakeRegistry.asked[:]
        times = [0, 0, 700]
        said = []
        code = check({"alpine:3.24@" + one: ["a.yml:1"], "registry.example/a/b@" + two: ["b.yml:2"],
                      "registry.example/c/d@" + one: ["c.yml:3"]}, said.append, nap,
                     lambda: times.pop(0) if len(times) > 1 else times[0], 600)
        text = "\n".join(said)
        expect(code == 2 and "ok       alpine:3.24@" + one in text and text.count("the check's 600-second deadline had passed") == 2
               and len(FakeRegistry.asked) == 2 and all("/library/alpine/" in p for p in FakeRegistry.asked),
               "no pin is asked once the deadline has passed, and each is named: %s %s\n%s" % (code, FakeRegistry.asked, text))
        # Nothing reached anything but the loopback: no request left without the fake's host.
        expect(all(p.startswith("/v2/") for p in FakeRegistry.asked), "only manifests were asked for: %s" % FakeRegistry.asked)
    finally:
        ENDPOINT.clear()
        ENDPOINT.update(held)
        server.shutdown()
        server.server_close()

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--list", action="store_true", help="the pins and where each is named, asking nothing")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--root", default=ROOT, help="the tree (default: this script's)")
    args = parser.parse_args()
    if args.self_test:
        return self_test(args.root)
    pins = tree_pins(args.root)
    if not pins:
        print("check-image-pins: no image is named by digest under %s - the wrong tree, or nothing read" % args.root)
        return 2
    if args.list:
        for reference in sorted(pins):
            print(reference)
            for place in pins[reference]:
                print("    " + place)
        print("%d pin(s) in %d place(s)" % (len(pins), sum(len(p) for p in pins.values())))
        return 0
    return check(pins)


if __name__ == "__main__":
    sys.exit(main())
