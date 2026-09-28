#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The project's OpenSSF Scorecard result on the update feed's site, with its badge.

The scorecard job (.gitlab/ci/scorecard.yml, build/ci/scorecard.sh) scores the
project once a week, in the pipeline of the schedule described "scorecard", and keeps
the result as its artifact, scorecard/scorecard.json. api.scorecard.dev takes results
only from GitHub's action, so the project publishes its own: this script adds

  scorecard/scorecard.json   Scorecard's result as it wrote it: every check, its score,
                             its reason and, with --show-details, what it found;
  scorecard/badge.svg        a badge with the aggregate score, for the README

to the site GitLab Pages serves at https://updates.progressiverobot.com, beside the
update feed, the apt and dnf repositories and the Helm repository. The package-repos
job (.gitlab/ci/package-repos.yml), which deploys that site, runs it after the others.

Where the result comes from, in order:
  --artifact DIR   the scorecard job's artifact, in the schedule's own pipeline, where
                   package-repos needs that job: this week's result;
  the live site    in every other pipeline that deploys the site (a push to master, a
                   release, the feed's schedule): the result the site serves now, read
                   back and published again, so that a deployment does not drop it.
                   Only a result that reads as Scorecard's is carried; the badge is
                   made again from it, never copied.
When neither gives one - no token yet (the job said "not configured"), or the site has
none - the site has no scorecard/, and this says so. A result it cannot fetch or read
is a WARNING and no scorecard/, never a failure: the feed and the repositories are the
site's purpose, and they deploy whatever this finds.

Usage:
  python3 build/ci/scorecard-pages.py --site public [--artifact scorecard] [--live URL]
  python3 build/ci/scorecard-pages.py --self-test
"""

import argparse
import functools
import http.client
import http.server
import importlib.util
import json
import os
import re
import shutil
import sys
import tempfile
import threading
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FOLDER = "scorecard"
RESULT = "scorecard.json"
BADGE = "badge.svg"
LABEL = "openssf scorecard"


def _site():
    """The feed's address, as build/generate-update-feed.py has it."""
    spec = importlib.util.spec_from_file_location("feed", os.path.join(ROOT, "build", "generate-update-feed.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.SITE


def check(document):
    """What is wrong with a Scorecard result (JSON, format v2), or []."""
    problems = []
    if not isinstance(document, dict):
        return ["it is not a JSON object"]
    if not isinstance(document.get("date"), str) or not re.match(r"^\d{4}-\d{2}-\d{2}T", document["date"]):
        problems.append("it has no date")
    repo = document.get("repo")
    if not isinstance(repo, dict) or not str(repo.get("name", "")).startswith("gitlab.com/"):
        problems.append("it names no gitlab.com repository")
    elif not re.match(r"^[0-9a-f]{40}$", str(repo.get("commit", ""))):
        problems.append("it names no commit")
    tool = document.get("scorecard")
    if not isinstance(tool, dict) or not str(tool.get("version", "")).startswith("v"):
        problems.append("it names no Scorecard version")
    score = document.get("score")
    if isinstance(score, bool) or not isinstance(score, (int, float)) or not -1 <= score <= 10:
        problems.append("its score is %r, not a number from -1 to 10" % (score,))
    if not isinstance(document.get("checks"), list):
        problems.append("it has no checks")
    return problems


def colour(score):
    if score < 0:
        return "#9f9f9f"
    if score >= 8:
        return "#4c1"
    if score >= 6:
        return "#97ca00"
    if score >= 4:
        return "#dfb317"
    if score >= 2:
        return "#fe7d37"
    return "#e05d44"


def badge(document):
    """The badge's SVG for a checked result."""
    score = float(document["score"])
    value = "?" if score < 0 else "%.1f" % score
    label_width = 6 * len(LABEL) + 12
    value_width = 7 * len(value) + 12
    width = label_width + value_width
    repo = document["repo"]
    title = "OpenSSF Scorecard %s for %s at %s, %s: %s / 10" % (
        document["scorecard"]["version"], repo["name"], repo["commit"][:12], document["date"], value)
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="20" role="img" aria-label="{label}: {value}">'
        '<title>{title}</title>'
        '<linearGradient id="s" x2="0" y2="100%"><stop offset="0" stop-color="#bbb" stop-opacity=".1"/>'
        '<stop offset="1" stop-opacity=".1"/></linearGradient>'
        '<clipPath id="r"><rect width="{w}" height="20" rx="3" fill="#fff"/></clipPath>'
        '<g clip-path="url(#r)"><rect width="{lw}" height="20" fill="#555"/>'
        '<rect x="{lw}" width="{vw}" height="20" fill="{fill}"/><rect width="{w}" height="20" fill="url(#s)"/></g>'
        '<g fill="#fff" text-anchor="middle" font-family="Verdana,Geneva,DejaVu Sans,sans-serif" font-size="11">'
        '<text x="{lx}" y="14">{label}</text><text x="{vx}" y="14">{value}</text></g></svg>\n').format(
            w=width, lw=label_width, vw=value_width, lx=label_width / 2, vx=label_width + value_width / 2,
            fill=colour(score), label=escape(LABEL), value=escape(value), title=escape(title))


def fetch(url):
    """(status, body): 0 and the error when the site cannot be reached."""
    request = urllib.request.Request(url, headers={"User-Agent": "hmailserver-scorecard-pages/1"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as e:
        return e.code, b""
    # OSError: no route, refused, reset, a timeout, TLS; HTTPException: an answer cut
    # short (IncompleteRead) or not HTTP; ValueError: an address urllib cannot use.
    except (urllib.error.URLError, OSError, http.client.HTTPException, ValueError) as e:
        return 0, ("%s: %s" % (type(e).__name__, e)).encode("utf-8", "replace")


def publish(site, artifact=None, live=None, say=print):
    """Adds scorecard/ to site when a result is found; returns where it came from, or None."""
    found, source = None, None
    if artifact:
        path = os.path.join(artifact, RESULT)
        if os.path.isfile(path):
            with open(path, "rb") as f:
                found, source = f.read(), "this pipeline's scorecard job (%s)" % path.replace(os.sep, "/")
        else:
            say("no %s in %s: the scorecard job wrote none this time (without its token it scores nothing)" % (RESULT, artifact))
    if found is None and live:
        url = "%s/%s/%s" % (live.rstrip("/"), FOLDER, RESULT)
        status, body = fetch(url)
        if status == 200:
            found, source = body, url
        elif status == 404:
            say("%s answers 404: no Scorecard result has been published yet" % url)
        else:
            say("WARNING: %s could not be read (%s): the site goes out without its Scorecard result until the next "
                "run" % (url, status or body.decode("utf-8", "replace")[:200]))
    if found is None:
        say("No Scorecard result to publish: the site has no %s/." % FOLDER)
        return None
    try:
        document = json.loads(found.decode("utf-8"))
    except ValueError as e:
        say("WARNING: the result from %s is not JSON (%s): not published" % (source, e))
        return None
    problems = check(document)
    if problems:
        say("WARNING: the result from %s is not a Scorecard result (%s): not published" % (source, "; ".join(problems)))
        return None
    folder = os.path.join(site, FOLDER)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, RESULT), "wb") as f:
        f.write(found)
    with open(os.path.join(folder, BADGE), "w", encoding="utf-8", newline="\n") as f:
        f.write(badge(document))
    say("Published %s/%s and %s/%s: %s scored %s / 10 on %s, from %s" % (
        FOLDER, RESULT, FOLDER, BADGE, document["repo"]["name"], document["score"], document["date"], source))
    return source


# ---------------------------------------------------------------- the self-test

SAMPLE = {"date": "2026-09-28T18:23:35Z",
          "repo": {"name": "gitlab.com/hmailserver/hmailserver", "commit": "6bc09509df0869ac444f635f3ccf354af6a3e004"},
          "scorecard": {"version": "v5.5.0", "commit": "c395761df6afe1a69e476bc60a013a94bcbc153f"},
          "score": 7.3, "checks": [{"details": None, "score": 10, "reason": "license file detected", "name": "License"}],
          "metadata": None}


def self_test():
    failures = []
    quiet = lambda line: None

    def expect(condition, what):
        if not condition:
            failures.append(what)

    expect(check(SAMPLE) == [], "Scorecard's own result reads: %s" % check(SAMPLE))
    for key, value, what in [("score", 11, "a score above 10"), ("score", "7.3", "a score that is text"),
                             ("score", True, "a score that is a boolean"), ("repo", {"name": "github.com/x/y"}, "a GitHub repository"),
                             ("checks", None, "no checks"), ("date", None, "no date")]:
        broken = dict(SAMPLE, **{key: value})
        expect(check(broken) != [], "%s is refused" % what)
    svg = badge(SAMPLE)
    try:
        tree = ET.fromstring(svg)  # nosemgrep: the badge this script just wrote
        texts = [t.text for t in tree.iter("{http://www.w3.org/2000/svg}text")]
    except ET.ParseError as e:
        texts = []
        failures.append("the badge is well-formed XML: %s" % e)
    expect(texts == [LABEL, "7.3"], "the badge reads '%s' and the score: %s" % (LABEL, texts))
    expect('fill="#97ca00"' in svg and 'aria-label="openssf scorecard: 7.3"' in svg, "7.3 is green, and said to a screen reader")
    expect(">?</text>" in badge(dict(SAMPLE, score=-1)), "-1, no check answered, is a question mark")
    expect("&lt;" in badge(dict(SAMPLE, date="<script>")), "the title's text is escaped")

    work = tempfile.mkdtemp(prefix="scorecard-pages-")
    served = os.path.join(work, "served")
    os.makedirs(os.path.join(served, FOLDER))

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *arguments):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Quiet, directory=served))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    live = "http://127.0.0.1:%d" % server.server_address[1]
    try:
        def site(name):
            path = os.path.join(work, name)
            os.makedirs(path)
            return path

        def written(public, name):
            try:
                with open(os.path.join(public, FOLDER, name), encoding="utf-8") as f:
                    return f.read()
            except OSError:
                return ""

        artifact = os.path.join(work, "artifact")
        os.makedirs(artifact)
        with open(os.path.join(artifact, RESULT), "w", encoding="utf-8") as f:
            json.dump(SAMPLE, f)
        public = site("from-artifact")
        expect(publish(public, artifact, live, quiet) is not None, "this pipeline's result is published")
        expect(written(public, RESULT) and json.loads(written(public, RESULT)) == SAMPLE, "the result goes out as Scorecard wrote it")
        expect(">7.3</text>" in written(public, BADGE), "with its badge")

        public = site("nothing")
        said = []
        expect(publish(public, os.path.join(work, "no-artifact"), live, said.append) is None, "with none anywhere, nothing")
        expect(not os.path.exists(os.path.join(public, FOLDER)) and any("has been published yet" in s for s in said)
               and not any(s.startswith("WARNING") for s in said),
               "and the site has no scorecard/, saying the live site has none, which is no warning: %s" % said)

        carried = dict(SAMPLE, score=9.1)
        with open(os.path.join(served, FOLDER, RESULT), "w", encoding="utf-8") as f:
            json.dump(carried, f)
        with open(os.path.join(served, FOLDER, BADGE), "w", encoding="utf-8") as f:
            f.write("<svg>a badge nobody checked</svg>")
        public = site("carried")
        expect(publish(public, None, live, quiet) == "%s/%s/%s" % (live, FOLDER, RESULT), "the live site's result is carried")
        expect(">9.1</text>" in written(public, BADGE), "and its badge made again from it, not copied")

        with open(os.path.join(served, FOLDER, RESULT), "w", encoding="utf-8") as f:
            f.write("<html>not a result</html>")
        public = site("broken-live")
        said = []
        expect(publish(public, None, live, said.append) is None and not os.path.exists(os.path.join(public, FOLDER)),
               "a live result that is not JSON is not carried")
        expect(any(s.startswith("WARNING") for s in said), "and it says so: %s" % said)

        with open(os.path.join(artifact, RESULT), "w", encoding="utf-8") as f:
            json.dump(dict(SAMPLE, score="high"), f)
        public = site("broken-artifact")
        expect(publish(public, artifact, None, quiet) is None, "an artifact that is not Scorecard's result is not published")

        # An answer cut short (http.client.IncompleteRead, which is not an OSError): a
        # warning, not a failure of the deployment.
        class Short(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Length", "100000")
                self.end_headers()
                self.wfile.write(b'{"date": "2026-09-28T')
                self.wfile.flush()
                self.close_connection = True

            def log_message(self, *arguments):
                pass

        short = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Short)
        threading.Thread(target=short.serve_forever, daemon=True).start()
        try:
            public = site("cut-short")
            said = []
            try:
                source = publish(public, None, "http://127.0.0.1:%d" % short.server_address[1], said.append)
                expect(source is None and any(s.startswith("WARNING") for s in said)
                       and not os.path.exists(os.path.join(public, FOLDER)),
                       "an answer cut short is a warning and no scorecard/: %s" % said)
            except Exception as e:
                failures.append("an answer cut short is a warning, not %s: %s" % (type(e).__name__, e))
        finally:
            short.shutdown()
            short.server_close()

        server.shutdown()
        server.server_close()
        public = site("unreachable")
        said = []
        expect(publish(public, None, live, said.append) is None and any(s.startswith("WARNING") for s in said),
               "a live site that cannot be reached is a warning, not a failure: %s" % said)
    finally:
        server.shutdown()
        server.server_close()
        shutil.rmtree(work, ignore_errors=True)

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--site", help="the site's directory (public)")
    parser.add_argument("--artifact", help="the scorecard job's artifact directory (scorecard)")
    parser.add_argument("--live", help="the site's address, to carry its result (default: the update feed's)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.site or not os.path.isdir(args.site):
        print("--site names no directory: %r" % args.site)
        return 2
    try:
        publish(args.site, args.artifact, args.live or _site())
    except Exception as e:
        # Whatever went wrong, the site deploys without scorecard/ rather than not at
        # all (the promise above): it carries the update feed and the package
        # repositories.
        shutil.rmtree(os.path.join(args.site, FOLDER), ignore_errors=True)
        print("WARNING: the Scorecard result was not published (%s: %s): the site goes out without it until the "
              "next run" % (type(e).__name__, e))
    return 0


if __name__ == "__main__":
    sys.exit(main())
