#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What a push mirror does not carry, exported from GitLab once a week.

A push mirror to a second forge carries the git refs, branches and tags, and nothing
else (hmailserver/docs/Mirroring.md). The forge-export job (.gitlab/ci/forge-export.yml),
started by the pipeline schedule whose description is "export", runs this script on
linix. It writes one archive of what the mirror leaves behind:

  project.json         the project's own settings as the API gives them
  issues.json          every issue, open and closed, confidential ones included, each
                       with its notes
  merge_requests.json  every merge request, each with its notes
  labels.json, milestones.json, boards.json
  releases.json        every release: its notes and the links to its files (the files
                       themselves are in the package registry, and are not copied)
  wiki.bundle          the wiki's git repository, all of its history and attachments,
                       as one file `git clone` reads
  MANIFEST.txt         the counts and each file's SHA-256

And, when a second token is given, GitLab's own project export: POST
/projects/:id/export, which needs a token with the api scope and the Maintainer role,
waited for and downloaded. It is the complete one (the repository, the uploads, the
issues and merge requests with everything attached), and the one another GitLab can
import.

The tokens are CI/CD variables of the project, protected and masked, and never in a
file of the tree:

  HM_EXPORT_TOKEN       required. A personal access token with read_api and
                        read_repository, of an account that can see confidential
                        issues (Reporter or above). Without it the script refuses and
                        says where the steps are.
  HM_EXPORT_FULL_TOKEN  optional. A personal access token with api, of a Maintainer:
                        the full project export. Without it that part is skipped, and
                        the log says so.

Each is sent as a header, to the API and to git, and never printed or put on a command
line. The job's log is public in this project, so it names counts and never an
issue's title or text.

--keep-dir holds the last --keep archives of each kind, the oldest removed: the job
points it at a directory the runner caches on linix, which is off GitLab.

Usage, from the repository root:
  python3 build/ci/forge-export.py --out DIR [--keep-dir DIR] [--keep N]
  python3 build/ci/forge-export.py --self-test     against an API in memory; no network, no token
"""

import argparse
import base64
import datetime
import hashlib
import http.server
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

API_BASE = "https://gitlab.com/api/v4"
PROJECT_ID = "86730559"
DOCS = "hmailserver/docs/Mirroring.md"
# A role that sees every confidential issue, and the one a project export needs.
REPORTER = 20
MAINTAINER = 40
# A token this close to its expiry date is named in the log each week.
WARN_DAYS = 30
FULL_WAIT_SECONDS = 30 * 60
POLL_SECONDS = 15
SMALL = "export"
FULL = "project-export"


class Refusal(Exception):
    """A reason to stop that the log states as it is, without a trace."""


def say(message):
    print(message, flush=True)


class Api:
    """The REST API with one token, sent as a header and never shown."""

    def __init__(self, base, project, token, opener=None):
        self.base = base.rstrip("/")
        self.project = str(project)
        self.token = token
        self.opener = opener or urllib.request.build_opener()

    def request(self, method, path, query=None, stream_to=None):
        url = self.base + path
        if query:
            url += "?" + urllib.parse.urlencode(query)
        request = urllib.request.Request(url, method=method, headers={
            # GitLab takes a personal access token as a bearer token, as it takes an
            # OAuth one.
            "Authorization": "Bearer " + self.token,
            "User-Agent": "hmailserver-forge-export",
            "Accept": "application/json",
        })
        attempts = 0
        while True:
            attempts += 1
            try:
                response = self.opener.open(request, timeout=120)
                break
            except urllib.error.HTTPError as error:
                # GitLab's rate limits and a passing fault of its own: wait, then again.
                if error.code in (429, 502, 503, 504) and attempts < 5:
                    time.sleep(min(60, 5 * attempts) if not QUIET else 0)
                    continue
                raise
        with response:
            headers = response.headers
            if stream_to is not None:
                with open(stream_to, "wb") as out:
                    shutil.copyfileobj(response, out, 1024 * 1024)
                return None, headers
            body = response.read()
        return (json.loads(body.decode("utf-8")) if body else None), headers

    def get(self, path, query=None):
        return self.request("GET", path, query)[0]

    def all_pages(self, path, query=None):
        """Every page of a list, following x-next-page, which GitLab sends even where
        it leaves out x-total."""
        items = []
        page = "1"
        while page:
            parameters = dict(query or {})
            parameters.update({"per_page": "100", "page": page})
            batch, headers = self.request("GET", path, parameters)
            items.extend(batch or [])
            page = (headers.get("x-next-page") or "").strip()
        return items

    def project_path(self, rest=""):
        return "/projects/%s%s" % (urllib.parse.quote(self.project, safe=""), rest)


def token_check(api, what, needs_api_scopes, needs_git, least_role, role_name):
    """The token is alive, has the scopes, and its account has the role. Returns the
    project as that account sees it."""
    try:
        me = api.get("/personal_access_tokens/self")
    except urllib.error.HTTPError as error:
        if not 400 <= error.code < 500:
            raise
        # Either a dead token or one of another kind, which this route does not
        # describe (GitLab.com answers an OAuth token with 400): /user tells the two
        # apart, and the project's
        # permissions below still decide.
        try:
            api.get("/user")
        except urllib.error.HTTPError as again:
            if again.code in (401, 403):
                raise Refusal("%s was refused by GitLab (%d): it has expired, been revoked, or is not a token. "
                              "Make a new one (%s)." % (what, again.code, DOCS))
            raise
        me = None
    if me is not None:
        scopes = set(me.get("scopes") or [])
        if not me.get("active", True):
            raise Refusal("%s is not active. Make a new one (%s)." % (what, DOCS))
        if not scopes & set(needs_api_scopes):
            raise Refusal("%s has the scopes %s; it needs %s (%s)." % (
                what, ", ".join(sorted(scopes)) or "none", " or ".join(needs_api_scopes), DOCS))
        if needs_git and not scopes & {"read_repository", "api"}:
            raise Refusal("%s has the scopes %s; it needs read_repository as well, to clone the wiki (%s)." % (
                what, ", ".join(sorted(scopes)) or "none", DOCS))
        expires = me.get("expires_at")
        if expires:
            left = (datetime.date.fromisoformat(expires) - datetime.date.today()).days
            if left <= WARN_DAYS:
                say("NOTE  %s expires on %s, in %d days. Make its successor now (%s)." % (what, expires, left, DOCS))
    try:
        project = api.get(api.project_path())
    except urllib.error.HTTPError as error:
        raise Refusal("%s cannot read project %s (HTTP %d)." % (what, api.project, error.code))
    permissions = project.get("permissions") or {}
    levels = [((permissions.get(key) or {}).get("access_level") or 0) for key in ("project_access", "group_access")]
    if max(levels) < least_role:
        raise Refusal("%s belongs to an account whose role in the project is below %s: %s (%s)." % (
            what, role_name, "it cannot see a confidential issue" if least_role == REPORTER
            else "GitLab refuses it a project export", DOCS))
    return project


def with_notes(api, kind, items):
    for item in items:
        item["notes"] = api.all_pages(api.project_path("/%s/%d/notes" % (kind, item["iid"])), {"sort": "asc"})
    return items


def git_environment(token):
    """git sends the token as a Basic header from its configuration in the environment:
    never in the URL, never on a command line, never in a file."""
    header = "Authorization: Basic " + base64.b64encode(("oauth2:" + token).encode("utf-8")).decode("ascii")
    env = dict(os.environ)
    env.update({
        "GIT_CONFIG_COUNT": "1",
        "GIT_CONFIG_KEY_0": "http.extraHeader",
        "GIT_CONFIG_VALUE_0": header,
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_ASKPASS": "true",
    })
    return env


def wiki_bundle(project, token, destination, work):
    """The wiki's repository as one bundle; False when the wiki has no page yet."""
    source = project["http_url_to_repo"]
    if not source.endswith(".git"):
        raise Refusal("the project's clone address does not end in .git: %s" % source)
    source = source[:-len(".git")] + ".wiki.git"
    mirror = os.path.join(work, "wiki.git")
    env = git_environment(token)
    run = subprocess.run(["git", "clone", "--quiet", "--mirror", source, mirror], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if run.returncode != 0:
        raise Refusal("git could not clone the wiki from %s: %s" % (
            source, run.stderr.decode("utf-8", "replace").strip()[-400:]))
    refs = subprocess.run(["git", "-C", mirror, "for-each-ref", "--count=1"], stdout=subprocess.PIPE)
    if not refs.stdout.strip():
        return False
    subprocess.run(["git", "-C", mirror, "bundle", "create", "--quiet", destination, "--all"],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["git", "bundle", "verify", "--quiet", destination], cwd=mirror, env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return True


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(directory, name, value):
    with open(os.path.join(directory, name), "w", encoding="utf-8", newline="\n") as out:
        json.dump(value, out, ensure_ascii=False, indent=1, sort_keys=True)
        out.write("\n")


def small_export(api, token, out, stamp, slug):
    """The archive of what the mirror does not carry; returns its path."""
    work = tempfile.mkdtemp(prefix="forge-export-")
    try:
        name = "%s-%s-%s" % (slug, SMALL, stamp)
        tree = os.path.join(work, name)
        os.makedirs(tree)
        project = token_check(api, "HM_EXPORT_TOKEN", ("read_api", "api"), True, REPORTER, "Reporter")
        issues = with_notes(api, "issues", api.all_pages(api.project_path("/issues"), {"scope": "all", "state": "all", "sort": "asc"}))
        merges = with_notes(api, "merge_requests", api.all_pages(api.project_path("/merge_requests"), {"scope": "all", "state": "all", "sort": "asc"}))
        labels = api.all_pages(api.project_path("/labels"), {"include_ancestor_groups": "true"})
        milestones = api.all_pages(api.project_path("/milestones"))
        boards = api.all_pages(api.project_path("/boards"))
        releases = api.all_pages(api.project_path("/releases"), {"order_by": "released_at", "sort": "asc"})
        project.pop("permissions", None)
        for file_name, value in (("project.json", project), ("issues.json", issues), ("merge_requests.json", merges),
                                 ("labels.json", labels), ("milestones.json", milestones), ("boards.json", boards),
                                 ("releases.json", releases)):
            write_json(tree, file_name, value)
        has_wiki = wiki_bundle(project, token, os.path.join(tree, "wiki.bundle"), work)
        counts = [
            ("issues", len(issues)),
            ("confidential issues", sum(1 for issue in issues if issue.get("confidential"))),
            ("issue notes", sum(len(issue["notes"]) for issue in issues)),
            ("merge requests", len(merges)),
            ("merge request notes", sum(len(merge["notes"]) for merge in merges)),
            ("labels", len(labels)),
            ("milestones", len(milestones)),
            ("boards", len(boards)),
            ("releases", len(releases)),
        ]
        lines = ["What %s held on %s, exported by build/ci/forge-export.py (%s)." % (
            project.get("path_with_namespace", api.project), stamp, DOCS), ""]
        lines += ["%-20s %d" % (label, number) for label, number in counts]
        lines.append("%-20s %s" % ("wiki", "wiki.bundle (git clone wiki.bundle)" if has_wiki else "no page yet"))
        lines.append("")
        for file_name in sorted(os.listdir(tree)):
            lines.append("%s  %s" % (sha256(os.path.join(tree, file_name)), file_name))
        with open(os.path.join(tree, "MANIFEST.txt"), "w", encoding="utf-8", newline="\n") as manifest:
            manifest.write("\n".join(lines) + "\n")
        os.makedirs(out, exist_ok=True)
        archive = os.path.join(out, name + ".tar.gz")
        # Written under another name and renamed when whole, so that a run cut short
        # leaves no half an archive in the artifact or the cache.
        partial = os.path.join(work, name + ".tar.gz.part")
        with tarfile.open(partial, "w:gz") as tar:
            tar.add(tree, arcname=name)
        shutil.move(partial, archive)
        for label, number in counts:
            say("      %-20s %d" % (label, number))
        say("      %-20s %s" % ("wiki", "bundled" if has_wiki else "no page yet"))
        return archive
    finally:
        shutil.rmtree(work, ignore_errors=True)


def full_export(api, out, stamp, slug):
    """GitLab's project export, started, waited for and downloaded; returns its path."""
    token_check(api, "HM_EXPORT_FULL_TOKEN", ("api",), False, MAINTAINER, "Maintainer")
    api.request("POST", api.project_path("/export"))
    say("      started GitLab's project export; waiting for it")
    waited = 0
    while True:
        status = (api.get(api.project_path("/export")) or {}).get("export_status")
        if status == "finished":
            break
        if status == "failed":
            raise Refusal("GitLab's project export failed. Its reason is on the project's Settings, General, "
                          "Advanced, Export project; the rest of the export is kept.")
        if waited >= FULL_WAIT_SECONDS:
            raise Refusal("GitLab's project export was still %s after %d minutes." % (status, waited // 60))
        time.sleep(POLL_SECONDS if not QUIET else 0)
        waited += POLL_SECONDS
    os.makedirs(out, exist_ok=True)
    archive = os.path.join(out, "%s-%s-%s.tar.gz" % (slug, FULL, stamp))
    # Downloaded under another name and renamed when whole: a download cut short is
    # removed, and never kept as one of the last four.
    partial = archive + ".part"
    try:
        api.request("GET", api.project_path("/export/download"), stream_to=partial)
        os.replace(partial, archive)
    finally:
        if os.path.exists(partial):
            os.remove(partial)
    say("      %-20s %d bytes" % ("project export", os.path.getsize(archive)))
    return archive


def keep(files, keep_dir, count):
    """Copy each archive into keep_dir, then keep the newest `count` of each kind."""
    os.makedirs(keep_dir, exist_ok=True)
    for path in files:
        target = os.path.join(keep_dir, os.path.basename(path))
        if os.path.abspath(path) != os.path.abspath(target):
            shutil.copy2(path, target)
    removed = []
    for kind in (SMALL, FULL):
        marker = "-%s-" % kind
        # The stamp is the name's tail, so the names sort by age.
        mine = sorted(name for name in os.listdir(keep_dir)
                      if marker in name and name.endswith(".tar.gz")
                      and not (kind == SMALL and "-%s-" % FULL in name))
        for name in mine[:-count] if count > 0 else []:
            os.remove(os.path.join(keep_dir, name))
            removed.append(name)
    return removed


QUIET = []


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", help="where the archive is written (the job's artifact)")
    parser.add_argument("--keep-dir", help="where the last --keep archives of each kind are kept")
    parser.add_argument("--keep", type=int, default=4)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.out:
        parser.error("--out is required")
    token = os.environ.get("HM_EXPORT_TOKEN", "").strip()
    if not token:
        say("REFUSED  HM_EXPORT_TOKEN is not set, so nothing was exported.\n"
            "         It is a CI/CD variable of the project, protected and masked: a personal access token\n"
            "         with read_api and read_repository, of an account that sees confidential issues.\n"
            "         The owner's steps are in %s, \"The owner's checklist\"." % DOCS)
        return 1
    if args.keep < 1:
        parser.error("--keep must be at least 1")
    if shutil.which("git") is None:
        say("REFUSED  git is not installed in this image; it clones the wiki.")
        return 1
    base = os.environ.get("CI_API_V4_URL", API_BASE)
    project = os.environ.get("CI_PROJECT_ID", PROJECT_ID)
    slug = os.environ.get("CI_PROJECT_PATH_SLUG", "progressiverobot-hmailserver")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    made = []
    say("The export of what a push mirror does not carry:")
    if not attempt(lambda: made.append(small_export(Api(base, project, token), token, args.out, stamp, slug))):
        return 1
    say("OK    %s" % os.path.basename(made[-1]))
    full_token = os.environ.get("HM_EXPORT_FULL_TOKEN", "").strip()
    full_failed = False
    if full_token:
        say("GitLab's project export:")
        if attempt(lambda: made.append(full_export(Api(base, project, full_token), args.keep_dir or args.out, stamp, slug))):
            say("OK    %s" % os.path.basename(made[-1]))
        else:
            # The week's archive is still kept, and is the job's artifact (artifacts and
            # cache are saved when the job fails too); the job fails so that the
            # schedule's owner hears of it.
            full_failed = True
    else:
        say("SKIP  GitLab's project export: HM_EXPORT_FULL_TOKEN is not set (%s says what it adds)." % DOCS)
    if args.keep_dir:
        removed = keep(made, args.keep_dir, args.keep)
        kept = sorted(name for name in os.listdir(args.keep_dir) if name.endswith(".tar.gz"))
        say("Kept in %s (the last %d of each kind): %s" % (args.keep_dir, args.keep, ", ".join(kept)))
        for name in removed:
            say("      removed %s" % name)
    if full_failed:
        say("FAILED  GitLab's project export; this week's archive of the issues, releases and wiki is kept all the same.")
        return 1
    return 0


def attempt(step):
    """Runs one part of the export; False, with the reason in the log, when it fails."""
    try:
        step()
        return True
    except Refusal as refusal:
        say("REFUSED  %s" % refusal)
    except urllib.error.HTTPError as error:
        say("FAILED  GitLab answered %d to %s" % (error.code, error.url.split("?")[0]))
    except urllib.error.URLError as error:
        say("FAILED  GitLab could not be reached: %s" % error.reason)
    return False


# ---------------------------------------------------------------- the self-test

def self_test():
    QUIET.append(True)
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    token = "glpat-SELFTEST-not-a-real-token-0123456789"
    full_token = "glpat-SELFTEST-full-token-9876543210"
    work = tempfile.mkdtemp(prefix="forge-export-test-")
    state = {"scopes": {token: ["read_api", "read_repository"], full_token: ["api"]},
             "levels": {token: 20, full_token: 40}, "export": "none", "posts": 0, "seen": [],
             "expires": {token: (datetime.date.today() + datetime.timedelta(days=200)).isoformat(),
                         full_token: (datetime.date.today() + datetime.timedelta(days=10)).isoformat()}}

    # The wiki: a bare repository with one page, where the project's address says.
    wiki = os.path.join(work, "remote", "hmailserver.wiki.git")
    seed = os.path.join(work, "seed")
    git = ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "init.defaultBranch=main"]
    subprocess.run(git + ["init", "--quiet", "--bare", wiki], check=True)
    subprocess.run(git + ["init", "--quiet", seed], check=True)
    with open(os.path.join(seed, "home.md"), "w") as page:
        page.write("# Home\n")
    subprocess.run(git + ["-C", seed, "add", "home.md"], check=True)
    subprocess.run(git + ["-C", seed, "commit", "--quiet", "-m", "home"], check=True)
    subprocess.run(git + ["-C", seed, "push", "--quiet", wiki, "HEAD:refs/heads/main"], check=True,
                   stderr=subprocess.DEVNULL)
    repository = "file:///" + os.path.join(work, "remote", "hmailserver.git").replace("\\", "/").lstrip("/")

    issues = [{"iid": n, "title": "issue %d" % n, "confidential": n == 3} for n in range(1, 151)]

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, code, value=None, headers=None, raw=None):
            body = raw if raw is not None else json.dumps(value).encode("utf-8")
            self.send_response(code)
            for key, item in (headers or {}).items():
                self.send_header(key, item)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def page(self, items, query):
            number = int(query.get("page", ["1"])[0])
            size = int(query.get("per_page", ["20"])[0])
            chunk = items[(number - 1) * size:number * size]
            more = number * size < len(items)
            self.reply(200, chunk, {"x-next-page": str(number + 1) if more else ""})

        def handle_any(self, method):
            parsed = urllib.parse.urlparse(self.path)
            query = urllib.parse.parse_qs(parsed.query)
            path = parsed.path
            given = (self.headers.get("Authorization") or "").partition("Bearer ")[2]
            state["seen"].append((method, path))
            if given not in state["scopes"]:
                return self.reply(401, {"message": "401 Unauthorized"})
            if path == "/api/v4/personal_access_tokens/self":
                if state["scopes"][given] is None:
                    return self.reply(404, {"message": "404 Not Found"})
                return self.reply(200, {"active": True, "scopes": state["scopes"][given], "expires_at": state["expires"][given]})
            if path == "/api/v4/user":
                return self.reply(200, {"id": 1})
            base = "/api/v4/projects/86730559"
            if path == base:
                return self.reply(200, {"id": 86730559, "path_with_namespace": "Progressiverobot/hmailserver",
                                        "http_url_to_repo": repository,
                                        "permissions": {"project_access": {"access_level": state["levels"][given]},
                                                        "group_access": None}})
            if path == base + "/issues":
                expect(query.get("scope") == ["all"] and query.get("state") == ["all"], "issues are asked for in every state")
                return self.page(issues, query)
            if path.startswith(base + "/issues/") and path.endswith("/notes"):
                iid = int(path.split("/")[-2])
                return self.page([{"id": iid * 10 + k, "body": "note"} for k in range(iid % 3)], query)
            if path == base + "/merge_requests":
                return self.page([{"iid": 1, "title": "mr"}], query)
            if path.startswith(base + "/merge_requests/"):
                return self.page([{"id": 1, "body": "mr note"}], query)
            if path in (base + "/labels", base + "/milestones", base + "/boards"):
                return self.page([{"id": 1}], query)
            if path == base + "/releases":
                return self.page([{"tag_name": "v%d" % n} for n in range(42)], query)
            if path == base + "/export" and method == "POST":
                if "api" not in state["scopes"][given]:
                    return self.reply(403, {"message": "403 Forbidden"})
                state["posts"] += 1
                state["export"] = "queued"
                return self.reply(202, {"message": "202 Accepted"})
            if path == base + "/export":
                # queued, then started, then finished: the script must wait through both.
                now = state["export"]
                state["export"] = {"queued": "started",
                                   "started": "failed" if state.get("fail_export") else "finished"}.get(now, now)
                return self.reply(200, {"export_status": now})
            if path == base + "/export/download":
                return self.reply(200, raw=b"\x1f\x8b project export bytes")
            return self.reply(404, {"message": "404 Not Found"})

        def do_GET(self):
            self.handle_any("GET")

        def do_POST(self):
            self.handle_any("POST")

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = "http://127.0.0.1:%d/api/v4" % server.server_address[1]
    saved_env = dict(os.environ)

    def run(env, argv):
        os.environ.clear()
        os.environ.update(saved_env)
        for key in ("HM_EXPORT_TOKEN", "HM_EXPORT_FULL_TOKEN"):
            os.environ.pop(key, None)
        os.environ.update({"CI_API_V4_URL": base, "CI_PROJECT_ID": "86730559",
                           "CI_PROJECT_PATH_SLUG": "progressiverobot-hmailserver"})
        os.environ.update(env)
        captured = io.StringIO()
        real = sys.stdout
        sys.stdout = captured
        try:
            code = main(argv)
        finally:
            sys.stdout = real
            os.environ.clear()
            os.environ.update(saved_env)
        return code, captured.getvalue()

    try:
        out = os.path.join(work, "out")
        kept = os.path.join(work, "kept")

        # Without the token: a refusal that says what to do, and nothing written.
        code, log = run({}, ["--out", out, "--keep-dir", kept])
        expect(code == 1 and "HM_EXPORT_TOKEN is not set" in log and DOCS in log, "no token: a clear refusal")
        expect(not os.path.exists(out), "no token: nothing written")
        expect(not state["seen"], "no token: GitLab not asked anything")

        # A token GitLab does not know.
        code, log = run({"HM_EXPORT_TOKEN": "glpat-unknown"}, ["--out", out])
        expect(code == 1 and "401" in log and "glpat-unknown" not in log, "a dead token is refused, and not printed")

        # A token without read_repository, and one whose account cannot see a confidential issue.
        state["scopes"][token] = ["read_api"]
        code, log = run({"HM_EXPORT_TOKEN": token}, ["--out", out])
        expect(code == 1 and "read_repository" in log, "a token that cannot clone the wiki is refused")
        state["scopes"][token] = ["read_api", "read_repository"]
        state["levels"][token] = 10
        code, log = run({"HM_EXPORT_TOKEN": token}, ["--out", out])
        expect(code == 1 and "confidential" in log, "a Guest's token is refused")
        state["levels"][token] = 20

        # The export itself: every page of 150 issues, with their notes; the wiki bundled.
        code, log = run({"HM_EXPORT_TOKEN": token}, ["--out", out, "--keep-dir", kept, "--keep", "2"])
        expect(code == 0, "the export succeeds: %s" % log)
        expect("SKIP  GitLab's project export" in log, "no full token: the project export is skipped, and said")
        expect(token not in log, "the token is never printed")
        archives = sorted(os.listdir(out)) if os.path.isdir(out) else []
        expect(len(archives) == 1 and archives[0].startswith("progressiverobot-hmailserver-export-"), "one archive: %s" % archives)
        if archives:
            with tarfile.open(os.path.join(out, archives[0])) as tar:
                names = set(os.path.basename(member.name) for member in tar.getmembers() if member.isfile())
                expect({"project.json", "issues.json", "merge_requests.json", "labels.json", "milestones.json",
                        "boards.json", "releases.json", "wiki.bundle", "MANIFEST.txt"} <= names, "the archive's files: %s" % names)
                member = next(m for m in tar.getmembers() if m.name.endswith("/issues.json"))
                exported = json.loads(tar.extractfile(member).read().decode("utf-8"))
                expect(len(exported) == 150 and exported[2]["confidential"], "every issue, confidential ones too")
                expect(sum(len(issue["notes"]) for issue in exported) == sum(n % 3 for n in range(1, 151)), "every issue's notes")
                member = next(m for m in tar.getmembers() if m.name.endswith("/project.json"))
                expect("permissions" not in json.loads(tar.extractfile(member).read().decode("utf-8")),
                       "the token's own permissions are not exported")
                member = next(m for m in tar.getmembers() if m.name.endswith("/wiki.bundle"))
                bundle = os.path.join(work, "check.bundle")
                with open(bundle, "wb") as copy:
                    copy.write(tar.extractfile(member).read())
                clone = subprocess.run(["git", "clone", "--quiet", bundle, os.path.join(work, "wikicheck")],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                expect(clone.returncode == 0 and os.path.isfile(os.path.join(work, "wikicheck", "home.md")),
                       "the wiki bundle clones, with its page")
                with open(os.path.join(work, "check.bundle"), "rb") as raw:
                    expect(token.encode() not in raw.read(), "the token is not in the bundle")
                member = next(m for m in tar.getmembers() if m.name.endswith("/MANIFEST.txt"))
                manifest = tar.extractfile(member).read().decode("utf-8")
                expect("confidential issues" in manifest and "issue 1" not in manifest, "the manifest has counts, not titles")
        expect("issue 1" not in log and "issue 3" not in log, "the log names no issue")
        expect(sorted(os.listdir(kept)) == archives, "the archive is kept")

        # A token of another kind, which /personal_access_tokens/self does not describe:
        # the project's permissions decide.
        oauth = "oauth-SELFTEST-token-of-another-kind"
        state["scopes"][oauth] = None
        state["levels"][oauth] = 20
        code, log = run({"HM_EXPORT_TOKEN": oauth}, ["--out", os.path.join(work, "oauth")])
        expect(code == 0 and oauth not in log, "a token of another kind with the role is taken: %s" % log)
        state["levels"][oauth] = 10
        code, log = run({"HM_EXPORT_TOKEN": oauth}, ["--out", os.path.join(work, "oauth")])
        expect(code == 1 and "confidential" in log, "and refused without it")

        # The full export: a Maintainer's api token, waited for through queued and started.
        state["seen"] = []
        time.sleep(1.1)
        code, log = run({"HM_EXPORT_TOKEN": token, "HM_EXPORT_FULL_TOKEN": full_token},
                        ["--out", out, "--keep-dir", kept, "--keep", "2"])
        expect(code == 0 and state["posts"] == 1, "the project export is started once: %s" % log)
        expect(state["seen"].count(("GET", "/api/v4/projects/86730559/export")) == 3,
               "the export's status is asked until it is finished, through queued and started")
        expect(("GET", "/api/v4/projects/86730559/export/download") in state["seen"], "and downloaded once finished")
        expect("expires on" in log, "a token about to expire is named")
        expect(full_token not in log and token not in log, "neither token is printed")
        full = [name for name in os.listdir(kept) if "-project-export-" in name]
        expect(len(full) == 1, "the project export is kept, not put in the artifact: %s" % os.listdir(kept))
        expect(not [name for name in os.listdir(out) if "-project-export-" in name], "the project export is not in the artifact")

        # A full token without the Maintainer role: the full part refused, clearly.
        state["levels"][full_token] = 30
        code, log = run({"HM_EXPORT_TOKEN": token, "HM_EXPORT_FULL_TOKEN": full_token}, ["--out", out])
        expect(code == 1 and "Maintainer" in log, "a Developer's full token is refused")
        expect("OK    progressiverobot-hmailserver-export-" in log, "and the week's archive is written all the same")
        state["levels"][full_token] = 40

        # GitLab's project export fails: the job fails, the week's archive is kept, and
        # nothing half-made is left behind.
        time.sleep(1.1)
        state["fail_export"] = True
        before = sorted(os.listdir(kept))
        code, log = run({"HM_EXPORT_TOKEN": token, "HM_EXPORT_FULL_TOKEN": full_token},
                        ["--out", out, "--keep-dir", kept, "--keep", "4"])
        state["fail_export"] = False
        after = sorted(os.listdir(kept))
        added = [name for name in after if name not in before]
        expect(code == 1 and "project export failed" in log and "is kept all the same" in log,
               "a failed project export fails the job, and says the rest is kept: %s" % log)
        expect(len(added) == 1 and "-export-" in added[0] and "-project-export-" not in added[0],
               "a failed project export still keeps the week's archive: %s" % added)
        expect(not [name for name in os.listdir(out) + after if name.endswith(".part")], "no half-made file is left")

        # The last two of each kind are kept, the oldest removed.
        time.sleep(1.1)
        code, log = run({"HM_EXPORT_TOKEN": token}, ["--out", out, "--keep-dir", kept, "--keep", "2"])
        small = sorted(name for name in os.listdir(kept) if "-export-" in name and "-project-export-" not in name)
        expect(code == 0 and len(small) == 2 and archives and archives[0] not in small and "removed" in log,
               "the oldest archive is removed past --keep: %s" % sorted(os.listdir(kept)))
        expect(len([name for name in os.listdir(kept) if "-project-export-" in name]) == 1,
               "keeping one kind leaves the other alone")

        # git sends the token from the environment, never from its command line.
        env = git_environment(token)
        expect(token not in " ".join(["git", "clone", "--quiet", "--mirror"]) and
               base64.b64decode(env["GIT_CONFIG_VALUE_0"].split()[-1]).decode().endswith(token), "git's header carries the token")
    finally:
        server.shutdown()
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        for failure in failures:
            print("FAIL  " + failure)
        return 1
    print("OK    forge-export.py: the refusals, the pages, the notes, the wiki bundle, the project export, the keeping, and no token printed.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
