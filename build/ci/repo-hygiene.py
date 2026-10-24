#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The repository-hygiene checks OpenSSF Scorecard ran, answered from GitLab.

Scorecard (the GitHub workflow scorecard.yml) stopped with the GitHub account on 18
September 2026, and it reads GitHub's API only; the scorecard job runs it over the
GitLab project since 6.3.6, and since 18 October 2026 .github/workflows/scorecard.yml
over the project's copy on GitHub. This is the part of it that GitLab
and the tree can answer, as the repo-hygiene job of .gitlab-ci.yml runs it. Six
checks, each PASS, FAIL or UNKNOWN (UNKNOWN when the answer is not public, and never
counted as a pass):

  Branch-Protection    master is protected, only Maintainers push or merge, and no
                       one force-pushes; batch* is protected; every v* tag is a
                       protected tag.
  Signed-Releases      the five newest v* releases each carry a signature asset
                       (Sigstore bundle, cosign bundle, .sig, .asc) - Scorecard's
                       rule - and each of their tags carries an SSH signature by a
                       key in .gitlab/allowed_signers, which git verifies here.
  Security-Policy      SECURITY.md, with a way to report a vulnerability privately.
  Pinned-Dependencies  every image in .gitlab-ci.yml and the local files it
                       includes (.gitlab/ci/*.yml), and every FROM in a Dockerfile,
                       pinned by digest (or, for the CI image, by the tag made from
                       its Dockerfile's hash that build/check-ci-image.py enforces);
                       every action a workflow under .github/workflows/ uses (the
                       copy on GitHub's) pinned by a full commit;
                       no pip install from an index without hashes, npm install,
                       or piped curl in those CI files or a shell script under
                       build/; a NuGet lock file beside every project that has
                       package references.
  Binary-Artifacts     no executable or compiled file tracked in git (by content,
                       not only by extension).
  Token-Permissions    no job that a merge request or an unprotected ref can start
                       reaches the project's own runners, asks for id_tokens or
                       secrets, or runs a CI script that handles CI_JOB_TOKEN or
                       another credential - judged by the rules each job runs by,
                       its own or those it extends, in .gitlab-ci.yml and the
                       files it includes, and for a job whose tags are default:'s
                       [$HM_RUNNER] by the workflow's rules, which choose its
                       runner; no credential written in those files;
                       CI_DEBUG_TRACE (which prints every variable into a public
                       job log) set nowhere. The project's job-token allowlist and
                       pipeline-variable settings are not public, and the check
                       says so rather than guessing.

And a seventh, of the tree alone, for the copy on GitHub:

  Allstar-Config       the .allstar/ folder holds allstar.yaml and a file for each
                       policy OpenSSF Allstar v4.6 reads in a repository, and no
                       other; each parses, has only Allstar's own keys, spelt as
                       Allstar spells them, with values of their types, and opts
                       in with the action log; scorecard.yaml names Scorecard's
                       checks alone. A file is read as Allstar's YAML library
                       reads it, and what that library could read otherwise than
                       it looks (a character beyond printable ASCII, YAML 1.1's
                       on, off, yes and no, an octal, hexadecimal or binary
                       number, a special float) is refused.

What is left out, and why, is in docs/ContinuousIntegration.md ("Security
scanning"): Code-Review, Maintained, Contributors, Dangerous-Workflow, Packaging,
SAST, CI-Tests, Fuzzing, CII-Best-Practices, Webhooks, License and
Dependency-Update-Tool, each either GitHub's alone or answered elsewhere.

Reads the GitLab API anonymously (no token is ever sent: every call below is one
GitLab answers for a public project) and the checkout it runs in. Needs Python 3,
and git with ssh-keygen for the tag signatures (without them that half is UNKNOWN).

Usage, from the root of a checkout:

  python3 build/ci/repo-hygiene.py [--out DIR]
  python3 build/ci/repo-hygiene.py --self-test

--self-test runs Token-Permissions, offline, over CI files made to pass it and
over CI files made to break it - a condition whose protected-ref clause sits
inside an OR, parentheses or a comment, rules the check could misread (a list at
another indentation, inline, by an alias, behind a merge key), tags and extends
written as block lists, id_tokens and the job token reached through a template,
an include it cannot open, default:'s [$HM_RUNNER] with a workflow that leaves a
merge request's or an unprotected ref's pipeline on the project's runners - and
Pinned-Dependencies' CI half over a CI file and
the file it includes, with an image by tag, a service by tag and an unhashed pip
install put in the included file; and Allstar-Config's YAML reading over 31
texts, each of which must give the value or the refusal its case names (a
comment after a value, a # inside quotes, a byte-order mark and CRLF, two
byte-order marks, a character beyond ASCII, null, an integer beyond 64 bits at
either end, one of 4,301 digits, a tag, an anchor), over 60 plain values, each of
YAML 1.1's words and the special floats in several cases, a number in each digit,
dot and sign it can begin with, and each first character it refuses, written out
rather than drawn from the check's tables, each of which must be refused with the
words its rule gives, and the check over the tree's own .allstar/ and over 55
changes to a folder that passes it, 50 of which it must fail with the words its
case names (a misspelt key, a key in another case, a value of each other type,
a threshold above 10 and below 0, a tab, a control character and a no-break
space, a flow sequence, a list in a list, a line indented past its block, a list
item's next key short of its first, the actions issue and fix, a policy not
opted in, an organisation-level key, a Scorecard check that does not exist,
actions.yaml, a subfolder, a file taken away); so that taking away any one rule,
or any one word, character or bound of a rule, fails it; it fails if any is
judged wrongly.

The project and the API come from GitLab CI's own variables (CI_API_V4_URL,
CI_PROJECT_ID, CI_PROJECT_URL) and default to gitlab.com's copy of this project.
Writes DIR/repo-hygiene.json, DIR/junit.xml (a test case per check, which GitLab
shows on every tier) and prints the table. Exits 1 when a check fails.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from xml.sax.saxutils import escape, quoteattr

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

API = os.environ.get("CI_API_V4_URL", "https://gitlab.com/api/v4").rstrip("/")
PROJECT = os.environ.get("CI_PROJECT_ID", "86730559")
REPOSITORY = os.environ.get("CI_PROJECT_URL", "https://gitlab.com/hmailserver/hmailserver") + ".git"

MAINTAINER = 40
RELEASES_CHECKED = 5


class Result:
    def __init__(self, name, what):
        self.name = name
        self.what = what
        self.status = "PASS"
        self.lines = []

    def fail(self, text):
        self.status = "FAIL"
        self.lines.append("FAIL     " + text)

    def unknown(self, text):
        if self.status == "PASS":
            self.status = "UNKNOWN"
        self.lines.append("UNKNOWN  " + text)

    def ok(self, text):
        self.lines.append("ok       " + text)

    def note(self, text):
        self.lines.append("note     " + text)


def api(path, params=None):
    """GET one API path anonymously, following pagination; returns the decoded JSON."""
    query = dict(params or {})
    query.setdefault("per_page", "100")
    results, page = None, 1
    while True:
        query["page"] = str(page)
        url = "%s/projects/%s/%s?%s" % (API, urllib.parse.quote(PROJECT, safe=""), path, urllib.parse.urlencode(query))
        request = urllib.request.Request(url, headers={"User-Agent": "hmailserver-repo-hygiene"})
        with urllib.request.urlopen(request, timeout=60) as response:
            data = json.load(response)
            next_page = response.headers.get("X-Next-Page", "")
        if not isinstance(data, list):
            return data
        results = (results or []) + data
        if not next_page:
            return results
        page = int(next_page)


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as f:
        return f.read()


def tracked_files():
    try:
        out = subprocess.run(["git", "-c", "safe.directory=*", "-C", ROOT, "ls-files", "-z"], capture_output=True, check=True).stdout
        return [p for p in out.decode("utf-8", "replace").split("\0") if p]
    except (OSError, subprocess.CalledProcessError):
        files = []
        for base, dirs, names in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d != ".git"]
            for n in names:
                files.append(os.path.relpath(os.path.join(base, n), ROOT).replace("\\", "/"))
        return files


# ---------------------------------------------------------------- Branch-Protection

def matches(pattern, name):
    return re.fullmatch(re.escape(pattern).replace(r"\*", ".*"), name) is not None


def check_branch_protection():
    r = Result("Branch-Protection", "master and batch* protected, Maintainers only; every v* tag protected")
    try:
        branches = api("protected_branches")
    except (urllib.error.URLError, OSError, ValueError) as e:
        r.unknown("the protected branches could not be read: %s" % e)
        return r
    for wanted, strict in (("master", True), ("batch-example", False)):
        rules = [b for b in branches if matches(b["name"], wanted)]
        label = "master" if strict else "batch*"
        if not rules:
            r.fail("%s is not a protected branch" % label)
            continue
        for b in rules:
            low = [a.get("access_level_description") or str(a.get("access_level"))
                   for kind in ("push_access_levels", "merge_access_levels")
                   for a in b.get(kind, [])
                   if a.get("user_id") or a.get("group_id") or a.get("deploy_key_id")
                   or (a.get("access_level") is not None and 0 < a["access_level"] < MAINTAINER)]
            if low:
                r.fail("%s (rule %r) lets %s push or merge" % (label, b["name"], ", ".join(low)))
            else:
                r.ok("%s (rule %r): push and merge by Maintainers only" % (label, b["name"]))
            if b.get("allow_force_push"):
                if strict:
                    r.fail("%s (rule %r) allows force push" % (label, b["name"]))
                else:
                    r.note("%s (rule %r) allows force push by Maintainers: a batch branch is re-cut while rows are"
                           " folded into it; master, which releases are cut from, does not" % (label, b["name"]))
            elif strict:
                r.ok("%s: no force push" % label)
    try:
        tags = [t for t in api("repository/tags", {"search": "^v"}) if t["name"].startswith("v")]
    except (urllib.error.URLError, OSError, ValueError) as e:
        r.unknown("the tags could not be read: %s" % e)
        return r
    unprotected = [t["name"] for t in tags if not t.get("protected")]
    if not tags:
        r.unknown("no v* tag to look at")
    elif unprotected:
        r.fail("%d of %d v* tags are not protected: %s" % (len(unprotected), len(tags), ", ".join(unprotected[:20])))
    else:
        r.ok("all %d v* tags are protected tags" % len(tags))
    r.note("who may create a protected tag is a project setting GitLab does not show anonymously"
           " (GET protected_tags answers 403); a maintainer checks it under Settings, Repository")
    return r


# ---------------------------------------------------------------- Signed-Releases

SIGNATURE_SUFFIXES = (".sigstore.json", ".sigstore", ".cosign.bundle", ".sig", ".asc", ".minisig", ".sign")
# Build provenance: the SLSA generator's hmailserver-v<v>.intoto.jsonl of 6.3.2 and
# 6.3.3, a Sigstore bundle under the only name Scorecard counts as provenance, and from
# 6.3.6 this project's own, hmailserver-<v>.intoto.sigstore.json, a Sigstore bundle
# holding the DSSE envelope (build/ci/release-chain.py, provenance_name), which
# Scorecard, reading the name alone, counts as one more signature; from the release
# after 6.3.7 that envelope alone again as hmailserver-<v>.intoto.jsonl
# (provenance_line_name), which Scorecard counts as provenance.
PROVENANCE_SUFFIXES = (".intoto.jsonl", ".intoto.sigstore.json")


def release_assets(names):
    """(signature assets, provenance assets) among a release's asset names."""
    return ([n for n in names if n.endswith(SIGNATURE_SUFFIXES)], [n for n in names if n.endswith(PROVENANCE_SUFFIXES)])


def verify_tags(names):
    """{tag: (verified, text)} for each tag, by git verify-tag against .gitlab/allowed_signers; None if git cannot."""
    signers = os.path.join(ROOT, ".gitlab", "allowed_signers")
    if not shutil.which("git") or not shutil.which("ssh-keygen") or not os.path.exists(signers):
        return None
    work = tempfile.mkdtemp(prefix="hm-tags-")
    try:
        repo = os.path.join(work, "tags.git")
        def git(*args, check=True):
            return subprocess.run(["git", "-C", repo] + list(args), capture_output=True, text=True, check=check)
        subprocess.run(["git", "init", "-q", "--bare", repo], check=True, capture_output=True)
        git("remote", "add", "origin", REPOSITORY)
        # The tag objects and their commits only: no tree, no file. About 80 KB.
        git("config", "remote.origin.promisor", "true")
        git("config", "remote.origin.partialclonefilter", "tree:0")
        git("config", "extensions.partialClone", "origin")
        refspecs = ["refs/tags/%s:refs/tags/%s" % (n, n) for n in names]
        fetched = git("-c", "credential.helper=", "fetch", "-q", "--depth=1", "--filter=tree:0", "--no-tags",
                      "origin", *refspecs, check=False)
        if fetched.returncode != 0:
            return {n: (False, "not fetched: " + fetched.stderr.strip()[:200]) for n in names}
        answers = {}
        for n in names:
            kind = git("cat-file", "-t", "refs/tags/" + n, check=False).stdout.strip()
            if kind != "tag":
                answers[n] = (False, "a lightweight tag: nothing to sign")
                continue
            v = git("-c", "gpg.ssh.allowedSignersFile=" + signers, "verify-tag", n, check=False)
            text = (v.stderr or v.stdout).strip().splitlines()
            answers[n] = (v.returncode == 0, text[0] if text else "exit %d" % v.returncode)
        return answers
    finally:
        shutil.rmtree(work, ignore_errors=True)


def check_signed_releases():
    r = Result("Signed-Releases", "the %d newest v* releases carry signatures, and their tags are signed" % RELEASES_CHECKED)
    try:
        releases = [x for x in api("releases") if x.get("tag_name", "").startswith("v")]
    except (urllib.error.URLError, OSError, ValueError) as e:
        r.unknown("the releases could not be read: %s" % e)
        return r
    releases.sort(key=lambda x: x.get("released_at") or "", reverse=True)
    newest = releases[:RELEASES_CHECKED]
    if not newest:
        r.fail("no v* release is published on GitLab")
        return r
    with_provenance = 0
    for rel in newest:
        names = [link["name"] for link in rel.get("assets", {}).get("links", [])]
        signatures, provenance = release_assets(names)
        with_provenance += 1 if provenance else 0
        if signatures:
            r.ok("%s: %d signature asset(s) for %d asset(s)%s" % (
                rel["tag_name"], len(signatures), len(names),
                ", and build provenance (%s)" % provenance[0] if provenance else ""))
        else:
            r.fail("%s: no signature asset among its %d asset(s)" % (rel["tag_name"], len(names)))
    r.note("%d of %d carry build provenance (in-toto); Scorecard scores provenance above signatures alone, and counts "
           "only a *.intoto.jsonl as provenance - 6.3.2's and 6.3.3's, the SLSA generator's Sigstore bundles under "
           "that name, and this project's own from the release after 6.3.7 - so 6.3.6's and 6.3.7's "
           "*.intoto.sigstore.json count there as signatures"
           % (with_provenance, len(newest)))
    tags = [rel["tag_name"] for rel in newest]
    verified = verify_tags(tags)
    if verified is None:
        r.unknown("git and ssh-keygen are needed to verify the tags' signatures, and one is missing")
    else:
        for tag in tags:
            good, text = verified[tag]
            if good:
                r.ok("tag %s: %s" % (tag, text))
            else:
                r.fail("tag %s: not verified against .gitlab/allowed_signers - %s" % (tag, text))
    return r


# ---------------------------------------------------------------- Security-Policy

def check_security_policy():
    r = Result("Security-Policy", "SECURITY.md says how to report a vulnerability privately")
    for candidate in ("SECURITY.md", "docs/SECURITY.md", ".gitlab/SECURITY.md"):
        if os.path.exists(os.path.join(ROOT, candidate)):
            text = read(candidate)
            break
    else:
        r.fail("no SECURITY.md at the root, in docs/ or in .gitlab/")
        return r
    words = len(text.split())
    # A private route: an address to write to, or GitLab's confidential issue. (Not
    # "security advisory": that is GitHub's route, which this project has lost.)
    routes = re.findall(r"(mailto:[^\s)\]>]+|confidential issue)", text, re.I)
    r.ok("%s: %d words" % (candidate, words))
    if words < 50:
        r.fail("%s is too short to be a policy (%d words)" % (candidate, words))
    if not re.search(r"vulnerab", text, re.I):
        r.fail("%s never says 'vulnerability'" % candidate)
    if not routes:
        r.fail("%s gives no address, link or confidential route for a report" % candidate)
    else:
        r.ok("a private route to report: %s" % ", ".join(sorted({x.split("?")[0].lower() for x in routes})[:3]))
    return r


# ---------------------------------------------------------------- Pinned-Dependencies

CI_IMAGE_TAG = re.compile(r"^hmailserver-ci/linux:\d+\.\d+-[0-9a-f]{12}$")


def ci_images(text):
    """Every image and service image .gitlab-ci.yml names, with its line number."""
    found = []
    context, context_indent = None, -1
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split(" #", 1)[0].rstrip()
        if not code.strip() or code.lstrip().startswith("#"):
            continue
        indent = len(code) - len(code.lstrip())
        if context and indent <= context_indent and not code.lstrip().startswith("-"):
            context = None
        m = re.match(r"^(\s*)(?:-\s*)?(image|services):\s*(\S*)\s*$", code)
        if m:
            value = m.group(3).strip("'\"")
            if m.group(2) == "image" and value and value[0] not in "{&*[":
                found.append((number, value))
                context = None
            else:
                context, context_indent = m.group(2), indent
            continue
        m = re.match(r"^\s*(?:-\s*)?name:\s*(\S+)\s*$", code)
        if m and context:
            found.append((number, m.group(1).strip("'\"")))
            if context == "image":
                context = None
    return found


def check_pinned_dependencies(files):
    r = Result("Pinned-Dependencies", "images, base images and installs pinned to a digest, hash or lock file")
    # .gitlab-ci.yml and every local file it includes: most of the jobs, and so most of
    # the images, are in .gitlab/ci/*.yml.
    ci_texts, unread = ci_files() if os.path.exists(os.path.join(ROOT, ".gitlab-ci.yml")) else ([], [])
    unpinned, count = [], 0
    for path, text in ci_texts:
        images = ci_images(text)
        count += len(images)
        for line, image in images:
            if "@sha256:" in image:
                continue
            if CI_IMAGE_TAG.match(image):
                continue
            if image.startswith("$"):
                continue
            unpinned.append("%s (%s line %d)" % (image, path, line))
    if not ci_texts:
        r.fail("no .gitlab-ci.yml")
    elif unpinned:
        r.fail("%d image(s) in the CI files named by a tag that can move: %s" % (len(unpinned), "; ".join(unpinned)))
    else:
        r.ok("all %d image(s) in .gitlab-ci.yml and the %d file(s) it includes pinned by digest, or the CI image by "
             "its Dockerfile's hash" % (count, len(ci_texts) - 1))
    for includer, what in unread:
        r.unknown("%s includes %s, which this check cannot read for its images" % (includer, what))

    dockerfiles = [f for f in files if re.search(r"(^|/)([^/]*\.)?Dockerfile(\.[^/]*)?$", f)]
    loose = []
    for path in dockerfiles:
        for i, line in enumerate(read(path).splitlines()):
            m = re.match(r"^\s*FROM\s+(?:--platform=\S+\s+)?(\S+)", line, re.I)
            if m and "@sha256:" not in m.group(1) and m.group(1).lower() != "scratch" and not m.group(1).startswith("$"):
                # A stage named earlier in the same file is not an image.
                if re.search(r"^\s*FROM\s+\S+\s+AS\s+%s\s*$" % re.escape(m.group(1)), read(path), re.I | re.M):
                    continue
                loose.append("%s line %d: %s" % (path, i + 1, m.group(1)))
    if loose:
        r.fail("base images not pinned by digest: %s" % "; ".join(loose))
    else:
        r.ok("every FROM in the %d Dockerfile(s) pinned by digest" % len(dockerfiles))

    # The GitHub workflows left the tree on 26 September 2026 (they are in the
    # history, at v6.3.3 for instance), and since 18 October 2026 it has one again,
    # .github/workflows/scorecard.yml, for the copy on GitHub: every action a
    # workflow uses is pinned by a full commit, as that repository's Actions settings
    # require too. A local action (./) and a docker:// image are not actions of
    # another repository.
    workflows = [f for f in files if f.startswith(".github/workflows/") and f.endswith((".yml", ".yaml"))]
    actions = 0
    loose_actions = []
    for path in workflows:
        for i, line in enumerate(read(path).splitlines()):
            m = re.match(r"^\s*(?:-\s*)?uses:\s*([^\s#]+)", line)
            if not m or m.group(1).startswith("./") or m.group(1).startswith("docker://"):
                continue
            actions += 1
            ref = m.group(1).rsplit("@", 1)[-1] if "@" in m.group(1) else ""
            if not re.fullmatch(r"[0-9a-f]{40}", ref):
                loose_actions.append("%s line %d: %s" % (path, i + 1, m.group(1)))
    if loose_actions:
        r.fail("%d GitHub action reference(s) not pinned by a full commit: %s" % (
            len(loose_actions), "; ".join(loose_actions[:10])))
    elif workflows:
        r.ok("all %d action references in the %d GitHub workflow(s) pinned by a full commit" % (actions, len(workflows)))

    ci_paths = [path for path, _ in ci_texts]
    scripts = ci_paths + [f for f in files if f not in ci_paths and (re.match(r"^build/.+\.sh$", f)
                          or (f.startswith("build/ci/") and f.endswith(".Dockerfile")) or f in dockerfiles)]
    risky = []
    for path in scripts:
        # A command continued over several lines is read as one, so that its
        # --require-hashes counts wherever it is written.
        logical, start, pending = [], 0, ""
        for i, line in enumerate(read(path).splitlines()):
            if not pending:
                start = i
            if line.rstrip().endswith("\\"):
                pending += line.rstrip()[:-1] + " "
                continue
            logical.append((start, pending + line))
            pending = ""
        for i, line in logical:
            code = line.split("#", 1)[0] if not path.endswith(".yml") else line
            if re.match(r"^\s*#", line):
                continue
            # --no-index installs from local files alone (cli-packages' own wheel), so
            # no index can hand it anything newer.
            if re.search(r"\bpip3?[\"']?\s+install\b", code) and "--require-hashes" not in code and "--no-index" not in code:
                risky.append("%s line %d: pip install without --require-hashes or --no-index" % (path, i + 1))
            if re.search(r"\bnpm\s+(install|i)\b", code):
                risky.append("%s line %d: npm install (npm ci installs from the lock file)" % (path, i + 1))
            if re.search(r"\b(curl|wget)\b[^|]*\|\s*(sudo\s+)?(ba|z|da)?sh\b", code):
                risky.append("%s line %d: a download piped into a shell" % (path, i + 1))
            if re.search(r"\bgo\s+install\s+\S+@latest\b", code):
                risky.append("%s line %d: go install @latest" % (path, i + 1))
    if risky:
        r.fail("installs that take whatever is newest: %s" % "; ".join(risky[:15]))
    else:
        r.ok("no pip install from an index without hashes, npm install or piped download in .gitlab-ci.yml, .gitlab/ci/*.yml,"
             " a shell script under build/ or a Dockerfile (%d files)" % len(scripts))

    projects = [f for f in files if f.endswith(".csproj")]
    missing = []
    with_references = 0
    for project in projects:
        if "<PackageReference" not in read(project):
            continue
        with_references += 1
        if os.path.dirname(project) + "/packages.lock.json" not in files:
            missing.append(project)
    if missing:
        r.fail("%d project(s) with package references and no packages.lock.json: %s" % (
            len(missing), "; ".join(missing)))
    else:
        r.ok("all %d project(s) with NuGet package references have a committed packages.lock.json" % with_references)

    undeclared = set()
    for path in files:
        if not path.endswith(".py"):
            continue
        for module in ("yaml", "playwright", "jsonschema", "requests", "paramiko"):
            if re.search(r"^\s*(import|from)\s+%s\b" % module, read(path), re.M):
                undeclared.add(module)
    if undeclared:
        r.note("Python modules imported by a script and named in no requirements file (so no scanner"
               " sees their versions): %s" % ", ".join(sorted(undeclared)))
    return r


# ---------------------------------------------------------------- Binary-Artifacts

# Scorecard's own list of extensions (checks/raw/binary_artifact.go), and the magic
# numbers of an executable or compiled file, so that a renamed binary is found too.
BINARY_EXTENSIONS = {
    "crx", "deb", "dex", "dey", "elf", "o", "a", "so", "macho", "iso", "class", "jar", "bundle", "dylib",
    "lib", "msi", "dll", "drv", "efi", "exe", "ocx", "pyc", "pyo", "par", "rpm", "wasm", "whl",
}
MAGIC = [
    (b"MZ", "a Windows executable (PE)"),
    (b"\x7fELF", "an ELF executable or object"),
    (b"\xca\xfe\xba\xbe", "a Java class or Mach-O universal binary"),
    (b"\xfe\xed\xfa\xce", "a Mach-O binary"), (b"\xfe\xed\xfa\xcf", "a Mach-O binary"),
    (b"\xce\xfa\xed\xfe", "a Mach-O binary"), (b"\xcf\xfa\xed\xfe", "a Mach-O binary"),
    (b"\x00asm", "a WebAssembly module"),
    (b"!<arch>\n", "a static library (ar)"),
    (b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", "an OLE compound file (MSI and others)"),
]


def check_binary_artifacts(files):
    r = Result("Binary-Artifacts", "no executable or compiled file tracked in git")
    flagged = []
    for path in files:
        full = os.path.join(ROOT, path)
        try:
            with open(full, "rb") as f:
                head = f.read(8)
        except OSError:
            continue
        what = None
        for magic, label in MAGIC:
            if head.startswith(magic):
                # "MZ" also opens text by chance; a PE file says where its header is.
                if magic == b"MZ" and not is_pe(full):
                    continue
                what = label
                break
        extension = path.rsplit(".", 1)[-1].lower() if "." in os.path.basename(path) else ""
        if what or extension in BINARY_EXTENSIONS:
            flagged.append("%s (%s)" % (path, what or "extension ." + extension))
    if flagged:
        r.fail("%d tracked file(s): %s" % (len(flagged), "; ".join(flagged[:30])))
    else:
        r.ok("none among %d tracked files" % len(files))
    return r


def is_pe(path):
    try:
        with open(path, "rb") as f:
            head = f.read(0x40)
            if len(head) < 0x40:
                return False
            offset = int.from_bytes(head[0x3C:0x40], "little")
            f.seek(offset)
            return f.read(4) == b"PE\0\0"
    except OSError:
        return False


# ---------------------------------------------------------------- Token-Permissions
#
# The CI files are read without a YAML library (the job's image has none), so the
# reading is held to what the check can be sure of, and a job written in a way it
# cannot read is refused rather than passed: a rule list given inline or by an
# alias, a merge key (<<:), tags or extends given by an alias, a !reference the
# files do not resolve, and an include it cannot open.

PROTECTED = '$CI_COMMIT_REF_PROTECTED == "true"'
# The variable default:'s tags name, which .gitlab-ci.yml's variables: set to linix and
# a workflow rule sets to GitLab's hosted small Linux runner for a pipeline the
# project's runners must take nothing of (docs/ContinuousIntegration.md, "Every job on
# the project's own runners").
RUNNER_VARIABLE = "HM_RUNNER"
HOSTED_RUNNER = "saas-linux-small-amd64"
NOT_A_MERGE_REQUEST = '$CI_PIPELINE_SOURCE != "merge_request_event"'
A_MERGE_REQUEST = '$CI_PIPELINE_SOURCE == "merge_request_event"'
A_SCHEDULE = '$CI_PIPELINE_SOURCE == "schedule"'
A_TAG = re.compile(r'\$CI_COMMIT_TAG =~ /.*/[a-z]*')


def top_key(line):
    """The key a line at the top level of a CI file opens, or None for a blank,
    commented, indented or document-marker line. A key with an anchor or a value
    on its line ("job: &x", "\"a job\":") is still the key."""
    if not line.strip() or line[0] in " \t#" or line.startswith(("---", "...")):
        return None
    m = re.match(r"""^(?:"([^"]*)"|'([^']*)'|([^\s:#'"][^:#]*?))[ \t]*:(?=\s|$)""", line)
    if not m:
        return line.strip()
    return next(g for g in m.groups() if g is not None).strip()


def ci_jobs(text):
    """{top-level key: its block's text} for the CI file, without a YAML library.
    The block's first line is the key's own line, so that a value written on it
    (an anchor, an alias) is seen."""
    blocks, name, body = {}, None, []
    for line in text.splitlines():
        key = top_key(line)
        if key is not None:
            if name is not None:
                blocks[name] = "\n".join(body)
            name, body = key, [line]
        elif name is not None:
            body.append(line)
    if name is not None:
        blocks[name] = "\n".join(body)
    return blocks


def ci_files():
    """.gitlab-ci.yml and the local files it includes, as [(path, text)], and the
    includes this check cannot read (anything but a local file or one of GitLab's
    own templates), as [(path, what)]."""
    found, unread, queue = [], [], [(None, ".gitlab-ci.yml")]
    while queue:
        includer, path = queue.pop(0)
        if path in [f for f, _ in found]:
            continue
        if "*" in path or not os.path.isfile(os.path.join(ROOT, path)):
            if includer is not None:
                unread.append((includer, "local: %s (no such file)" % path))
            continue
        text = read(path)
        found.append((path, text))
        body = ci_jobs(text).get("include")
        if body is None:
            continue
        lines = body.splitlines()
        inline = re.split(r"(?:^|\s)#", lines[0].split(":", 1)[1], maxsplit=1)[0].strip()
        entries = [inline] if inline else []
        level = None
        for line in lines[1:]:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if level is None:
                level = indent_of(line)
            if indent_of(line) > level:
                continue
            m = re.match(r"^\s*-\s*(.*?)\s*$", line)
            entries.append(m.group(1) if m else line.strip())
        for entry in entries:
            entry = re.split(r"(?:^|\s)#", entry, maxsplit=1)[0].strip()
            local = re.match(r"^local:\s*(\S+)$", entry)
            if local:
                queue.append((path, local.group(1).strip("'\"").lstrip("/")))
            elif re.match(r"^template:\s*\S+$", entry):
                continue
            elif re.match(r"""^['"]?[^\s:'"\[{]+\.ya?ml['"]?$""", entry):
                queue.append((path, entry.strip("'\"").lstrip("/")))
            else:
                unread.append((path, entry))
    return found, unread


def mask_literals(expr):
    """expr with what is inside its quoted strings and its regular expressions
    blanked, so that a parenthesis or an operator in there is not taken for one."""
    out, i = list(expr), 0
    while i < len(expr):
        c = expr[i]
        if c in "\"'":
            j = i + 1
            while j < len(expr) and expr[j] != c:
                j += 2 if expr[j] == "\\" else 1
            for k in range(i + 1, min(j, len(expr))):
                out[k] = "_"
            i = j + 1
            continue
        if expr.startswith(("=~", "!~"), i):
            j = i + 2
            while j < len(expr) and expr[j] == " ":
                j += 1
            if j < len(expr) and expr[j] == "/":
                k = j + 1
                while k < len(expr) and expr[k] != "/":
                    k += 2 if expr[k] == "\\" else 1
                for m in range(j + 1, min(k, len(expr))):
                    out[m] = "_"
                i = k + 1
                continue
            i = j
            continue
        i += 1
    return "".join(out)


def split_top(expr, op):
    """expr cut at each op (|| or &&) outside parentheses and literals."""
    masked = mask_literals(expr)
    parts, depth, start, i = [], 0, 0, 0
    while i < len(expr):
        c = masked[i]
        depth += c == "("
        depth -= c == ")"
        if depth == 0 and masked.startswith(op, i):
            parts.append(expr[start:i].strip())
            i += len(op)
            start = i
            continue
        i += 1
    parts.append(expr[start:].strip())
    return parts


def unwrap(expr):
    """expr without the parentheses, if any, that enclose the whole of it."""
    expr = expr.strip()
    while expr.startswith("(") and expr.endswith(")"):
        masked, depth = mask_literals(expr), 0
        for i, c in enumerate(masked):
            depth += c == "("
            depth -= c == ")"
            if depth == 0 and i < len(expr) - 1:
                return expr
        expr = expr[1:-1].strip()
    return expr


def normal(clause):
    """A clause with its spacing made one way: "$A==\"x\"" is "$A == \"x\""."""
    masked = mask_literals(clause)
    out, i = "", 0
    for m in re.finditer(r"\s*(==|!=|=~|!~)\s*", masked):
        out += clause[i:m.start()] + " " + m.group(1) + " "
        i = m.end()
    return " ".join((out + clause[i:]).split())


def alternatives(condition):
    """A rule's condition as the alternatives it is the OR of, each unwrapped."""
    out = []
    for a in split_top(unwrap(condition), "||"):
        inner = unwrap(a)
        out += alternatives(inner) if inner != a else [a]
    return out


def conjuncts(alternative):
    """The clauses an alternative is the AND of, each normalised. A parenthesised
    AND is opened into its clauses; a parenthesised OR stays one clause, which
    guarantees none of its parts."""
    out = []
    for part in split_top(alternative, "&&"):
        inner = unwrap(part)
        if inner != part and len(split_top(inner, "||")) == 1:
            out += conjuncts(inner)
        else:
            out.append(normal(part))
    return out


def admits(rules):
    """Why these rules can create a job for code nobody has reviewed - a merge
    request's, or an unprotected ref's - or None when they cannot. A rule that
    creates the job must ask for a protected ref in each of its alternatives, as
    a clause every pipeline it admits must meet (not inside an OR), and shut out
    a merge request: by saying so, by asking for a schedule or a tag (neither is a
    merge request's pipeline), or by an earlier rule that sends every merge
    request's pipeline to never."""
    if rules is None:
        return "no rules: every pipeline, a merge request's included"
    mr_closed = False
    for rule in rules:
        if rule.get("unreadable"):
            return "rules this check cannot read: %s" % rule["unreadable"]
        condition = rule["if"]
        if rule["when"] == "never":
            if not rule["narrowed"]:
                if not condition:
                    return None
                if any(normal(a) == A_MERGE_REQUEST for a in alternatives(condition)):
                    mr_closed = True
            continue
        for a in alternatives(condition) if condition else [""]:
            clauses = conjuncts(a) if a else []
            if PROTECTED not in clauses:
                return "a rule that does not ask for a protected ref: %s" % (condition or "(no condition)")
            if not (mr_closed or NOT_A_MERGE_REQUEST in clauses or A_SCHEDULE in clauses
                    or any(A_TAG.fullmatch(c) for c in clauses)):
                return "a rule a merge request's pipeline can meet: %s" % condition
    return None


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


def job_key(body, key):
    """(the value on a job-level key's own line, the lines of its block) or None
    when the job has no such key. The job's keys are at the indentation of its
    first key; a block ends at the next line that is not deeper and is not an item
    of a sequence written at the key's own indentation."""
    lines = body.splitlines()[1:]
    keys = [indent_of(l) for l in lines if l.strip() and not l.lstrip().startswith("#")]
    if not keys:
        return None
    level = keys[0]
    for i, line in enumerate(lines):
        m = re.match(r"^ {%d}%s:(?=\s|$)(.*)$" % (level, re.escape(key)), line)
        if not m:
            continue
        inline = m.group(1).strip()
        if not inline.startswith(("'", '"')):
            inline = re.split(r"(?:^|\s)#", inline, maxsplit=1)[0].strip()
        block = []
        for following in lines[i + 1:]:
            if following.strip() and not following.lstrip().startswith("#"):
                if indent_of(following) < level or (indent_of(following) == level and not following.lstrip().startswith("- ")):
                    break
            block.append(following)
        return inline, block
    return None


def sequence(block):
    """The items of a block sequence, each as its lines with the dash made a space,
    or None when the block is not one sequence the check can read."""
    items, level = [], None
    for line in block:
        if not line.strip() or line.lstrip().startswith("#"):
            if items:
                items[-1].append(line)
            continue
        if level is None:
            if not line.lstrip().startswith("-"):
                return None
            level = indent_of(line)
        if indent_of(line) == level and re.match(r"^ *-(\s|$)", line):
            items.append([line.replace("-", " ", 1)])
        elif indent_of(line) > level and items:
            items[-1].append(line)
        else:
            return None
    return items


def values(body, key):
    """The scalar values of a job-level key - one on its line, a [flow] list or a
    block sequence - or None when the key is written in a way the check cannot
    read (an alias, a mapping); [] when the job has no such key."""
    found = job_key(body, key)
    if found is None:
        return []
    inline, block = found
    if inline:
        if inline.startswith(("*", "&", "{", "!")):
            return None
        if inline.startswith("["):
            if not inline.endswith("]"):
                return None
            return [v.strip().strip("'\"") for v in inline[1:-1].split(",") if v.strip()]
        return [inline.strip("'\"")]
    items = sequence(block)
    if items is None:
        return None
    out = []
    for item in items:
        text = "\n".join(item).strip()
        text = re.split(r"(?:^|\s)#", text, maxsplit=1)[0].strip()
        if not text or text.startswith(("*", "&", "{", "!", "[")) or "\n" in text:
            return None
        out.append(text.strip("'\""))
    return out


def scalar(first, continuation):
    """The text of a YAML scalar that starts after "key:" on one line and may go
    on over deeper lines: plain (a comment ends it), quoted, or a block scalar."""
    text = first.strip()
    rest = [l.strip() for l in continuation]
    if re.fullmatch(r"[|>][+-]?", text):
        return " ".join(l for l in rest if l)
    if text.startswith("'"):
        m = re.match(r"'((?:[^']|'')*)'", " ".join([text] + rest))
        return m.group(1).replace("''", "'") if m else None
    if text.startswith('"'):
        m = re.match(r'"((?:[^"\\]|\\.)*)"', " ".join([text] + rest))
        return re.sub(r"\\(.)", r"\1", m.group(1)) if m else None
    words = []
    for piece in [text] + rest:
        code = re.split(r"(?:^|\s)#", piece, maxsplit=1)
        if code[0].strip():
            words.append(code[0].strip())
        if len(code) > 1:
            break
    return " ".join(words)


def rule_of(item):
    """One item of a rules list as {"if", "when", "narrowed", "runner", "unreadable"}:
    runner is the HM_RUNNER its variables: set, or None."""
    keys = [l for l in item if l.strip() and not l.lstrip().startswith("#")]
    rule = {"if": "", "when": "on_success", "narrowed": False, "runner": None, "unreadable": None}
    if not keys:
        rule["unreadable"] = "an empty rule"
        return rule
    level = indent_of(keys[0])
    for i, line in enumerate(keys):
        if indent_of(line) != level:
            continue
        m = re.match(r"^\s*([A-Za-z_<]+):(?=\s|$)(.*)$", line)
        if not m:
            rule["unreadable"] = "a rule written as %s" % line.strip()
            continue
        key, value = m.group(1), m.group(2)
        deeper = []
        for following in keys[i + 1:]:
            if indent_of(following) <= level:
                break
            deeper.append(following)
        if key == "if":
            text = scalar(value, deeper)
            if text is None:
                rule["unreadable"] = "if: %s" % value.strip()
            rule["if"] = text or ""
        elif key == "when":
            rule["when"] = value.split("#", 1)[0].strip().strip("'\"")
        elif key in ("changes", "exists"):
            rule["narrowed"] = True
        elif key == "<<" or value.strip().startswith("*"):
            rule["unreadable"] = "%s:%s" % (key, value)
        elif key == "variables":
            if value.split("#", 1)[0].strip():
                if RUNNER_VARIABLE in value:
                    rule["unreadable"] = "variables: %s" % value.strip()
                continue
            for line in deeper:
                m = re.match(r"""^\s*["']?%s["']?\s*:(.*)$""" % RUNNER_VARIABLE, line)
                if m:
                    rule["runner"] = scalar(m.group(1), [])
    return rule


def check_token_permissions(files):
    r = Result("Token-Permissions", "no credential in reach of code nobody has reviewed")
    path = ".gitlab-ci.yml"
    if not os.path.exists(os.path.join(ROOT, path)):
        r.fail("no .gitlab-ci.yml")
        return r
    sources, unread = ci_files()
    for where, what in unread:
        r.fail("%s includes %s, which this check cannot read: its jobs would go unjudged" % (where, what))
    text = "\n".join(t for _, t in sources)
    jobs = {}
    for _, t in sources:
        jobs.update(ci_jobs(t))
    reserved = {"stages", "workflow", "default", "variables", "include"}
    real = [n for n in jobs if n not in reserved and not n.startswith(".")]

    def parents(name):
        found = values(jobs.get(name, ""), "extends")
        return found or []

    def extends(name, depth=0):
        chain = list(parents(name))
        if depth < 5:
            for p in parents(name):
                chain += extends(p, depth + 1)
        return chain

    def unreadable(name):
        """What of a job (or what it extends) this check cannot follow, or None."""
        for n in [name] + extends(name):
            body = jobs.get(n)
            if body is None:
                return "it extends %s, which none of the CI files defines" % n
            head = body.splitlines()[0] if body else ""
            if ":" not in head:
                return "%s is not a key the check can read" % n
            first = head.split(":", 1)[1].strip()
            if first and not first.startswith(("#", "&")):
                return "%s is given as %s" % (n, first)
            if job_key(body, "<<") is not None:
                return "%s merges a YAML anchor (<<:)" % n
            if values(body, "extends") is None:
                return "%s's extends is not a list of names" % n
        return None

    def rules_of(name, depth=0):
        """The rules a job runs by: its own, else those of the LAST parent in its
        extends that has any - GitLab merges an extends list in order, a later entry
        over an earlier one, and a list such as rules is replaced whole - each a
        dict; None when there are none."""
        found = job_key(jobs.get(name, ""), "rules")
        if found is not None:
            inline, block = found
            items = None if inline else sequence(block)
            if not items:
                return [{"unreadable": "rules: %s" % (inline or "(not a list)")}]
            rules = []
            for item in items:
                ref = re.match(r"^\s*!reference\s*\[\s*([^,\]\s]+)\s*,\s*rules\s*\]\s*$",
                               re.split(r"\s#", "\n".join(item).strip(), maxsplit=1)[0])
                if ref:
                    resolved = rules_of(ref.group(1), depth + 1) if depth < 5 and ref.group(1) in jobs else None
                    rules += resolved if resolved is not None else [{"unreadable": "!reference [%s, rules]" % ref.group(1)}]
                    continue
                rules.append(rule_of(item))
            return rules
        if depth < 5:
            for p in reversed(parents(name)):
                found = rules_of(p, depth + 1)
                if found is not None:
                    return found
        return None

    def judged(name):
        """Why a job can be started for unreviewed code, or None."""
        return ("its YAML: %s" % unreadable(name)) if unreadable(name) else admits(rules_of(name))

    # A tag of one of the project's runners, or a variable, which could be one: default:'s
    # [$HM_RUNNER], or a class's switch (HM_RUNNER_BUILD and the rest), which is linix in
    # the CI file and which the owner's protected project variable may set to one of
    # GitLab's hosted runners - a job on a switch is judged by its own rules, as a job on
    # linix is. The runners are registered with more tags than their names (glab api
    # runners/<id>, 26 September 2026): linix with linux, docker and linix, bench150 with
    # windows, bench and bench150, and S1-WINBENCH since 13 October 2026 with windows and
    # bench-s1vm. A job asking for any of them can land on one.
    own = re.compile(r"\b(linix|linux|docker|bench150|bench-s1vm|bench|windows)\b|\$", re.I)
    # default:'s tags, which a job that names none - itself or through extends - takes.
    default_tags = values(jobs.get("default", ""), "tags")
    # A job whose tags are [$HM_RUNNER] runs where the workflow's rules leave HM_RUNNER:
    # on GitLab's hosted runner for a rule that sets it so - such a pipeline reaches no
    # machine of the project's - and on the runner .gitlab-ci.yml's variables: name for
    # every other rule that makes a pipeline, which must then ask for a protected ref
    # and shut out a merge request, as a job's own rules must. A job that sets
    # HM_RUNNER itself, or one of its templates, is judged by its own rules instead.
    global_runner = values(jobs.get("variables", ""), RUNNER_VARIABLE)
    global_runner = global_runner[0] if global_runner and len(global_runner) == 1 else None
    workflow = rules_of("workflow")
    if workflow is None:
        workflow_why = "the workflow has no rules"
    else:
        project_rules = []
        for rule in workflow:
            rule = dict(rule)
            if (rule.get("runner") or global_runner) == HOSTED_RUNNER:
                # This pipeline's jobs of [$HM_RUNNER] go to GitLab's runner: none of
                # them is made on the project's, as if the rule made no pipeline.
                rule["when"] = "never"
            project_rules.append(rule)
        workflow_why = admits(project_rules)

    def sets_runner(name):
        return any(re.search(r"""(?:^|[\s{,])["']?%s["']?\s*:""" % RUNNER_VARIABLE,
                             re.sub(r"(?m)(?:^|\s)#.*$", "", jobs.get(n, "")))
                   for n in [name] + extends(name))

    guard = jobs.get(".own-runners", "")
    guard_ok = judged(".own-runners") is None and PROTECTED in guard
    if guard_ok:
        r.ok(".own-runners creates a job only for a protected ref, and never for a merge request")
    else:
        r.fail(".own-runners no longer demands a protected ref that is not a merge request's")
    on_own, exposed, through_guard, through_workflow, unclear = [], [], 0, 0, []
    for name in real:
        # A job whose extends, key line or merge keys the check cannot follow may take
        # its tags and rules from anywhere: it is refused whatever its tags look like.
        problem = unreadable(name)
        if problem:
            unclear.append("%s (%s)" % (name, problem))
            continue
        tags, named = [], False
        for n in [name] + extends(name):
            found = values(jobs.get(n, ""), "tags")
            named = named or found is None or job_key(jobs.get(n, ""), "tags") is not None
            # Tags the check cannot read may be the project's own runners.
            tags += ["linix"] if found is None else found
        if not named:
            # Tags the check cannot read may be the project's own runners.
            tags = ["linix"] if default_tags is None else default_tags
        if not any(own.search(t) for t in tags):
            continue
        on_own.append(name)
        if tags == ["$" + RUNNER_VARIABLE] and not sets_runner(name):
            if workflow_why:
                exposed.append("%s (it runs where the workflow's rules leave %s, and %s)" % (
                    name, RUNNER_VARIABLE, workflow_why))
            else:
                through_workflow += 1
            continue
        why = judged(name)
        if why:
            exposed.append("%s (%s)" % (name, why))
        elif rules_of(name) == rules_of(".own-runners"):
            through_guard += 1
    if unclear:
        r.fail("job(s) written in a way this check cannot read, and so cannot clear: %s" % "; ".join(unclear))
    if exposed:
        r.fail("job(s) on the project's own runners that a merge request or an unprotected ref can start: %s"
               % "; ".join(exposed))
    else:
        r.ok("all %d jobs on linix or bench150 run only for a protected ref and never for a merge request:"
             " %d by .own-runners, %d by rules of their own that ask the same, %d by the workflow's rules, which"
             " give every other pipeline GitLab's hosted runner (%s)" % (
                 len(on_own), through_guard, len(on_own) - through_guard - through_workflow, through_workflow,
                 RUNNER_VARIABLE))

    def runners_of(name):
        """The jobs that run what a block holds: the job itself, every job that
        extends it or takes a part of it by !reference, and every job for the
        global blocks (variables, default)."""
        if name in reserved:
            return list(real)
        out = [name] if name in real else []
        for n in real:
            if n != name and (name in extends(n) or re.search(r"!reference\s*\[\s*%s\s*," % re.escape(name), jobs[n])):
                out.append(n)
        return out

    # A script that handles a credential is judged by the jobs that run it: each must
    # be one no merge request and no unprotected ref can start.
    # GitLab's secret-detection template is configured by SECRET_DETECTION_* switches,
    # which name no secret.
    credential = re.compile(r"\$\{?(CI_JOB_TOKEN|CI_REGISTRY_PASSWORD|CI_DEPLOY_PASSWORD|CI_JOB_JWT\w*|"
                            r"(?!SECRET_DETECTION_)[A-Z0-9_]*(?:TOKEN|SECRET)[A-Z0-9_]*)\}?")
    scripts = [f for f in files if re.match(r"^build/ci/[^/]+\.(sh|ps1)$", f)]
    handled, unsafe = [], []
    for s in [f for f, _ in sources] + scripts:
        found = []
        current = None
        for i, line in enumerate(read(s).splitlines()):
            key = top_key(line)
            if key is not None:
                current = key
            if re.match(r"^\s*#", line):
                continue
            for m in credential.finditer(line):
                found.append((i + 1, m.group(1), current))
        if not found:
            continue
        if s.endswith((".yml", ".yaml")):
            holders = {job for _, _, job in found}
        else:
            name = s.split("/")[-1]
            holders = {n for n in jobs if re.search(r"build[/\\]ci[/\\]" + re.escape(name) + r"\b", jobs[n])}
        runners = sorted({n for h in holders if h is not None for n in runners_of(h)})
        where = ", ".join("line %d: %s" % (line, var) for line, var, _ in found[:4])
        if not runners:
            unsafe.append("%s (%s), which no job runs" % (s, where))
            continue
        bad = [n for n in runners if judged(n)]
        if bad:
            unsafe.append("%s (%s), run by %s" % (s, where, ", ".join(bad)))
        else:
            handled.append("%s (%s)" % (s, ", ".join(runners)))
    if unsafe:
        r.fail("CI scripts handle a credential where unreviewed code can reach it: %s" % "; ".join(unsafe[:10]))
    else:
        r.ok("no CI script handles CI_JOB_TOKEN or any *TOKEN* or *SECRET* variable outside a protected ref's"
             " jobs (%d files)" % (len(sources) + len(scripts)))
    for h in handled:
        r.note("handles a credential, in jobs a protected ref alone can start: %s" % h)

    for name in real:
        # id_tokens and secrets are inherited through extends like any other key.
        if any(job_key(jobs.get(n, ""), k) is not None for n in [name] + extends(name) for k in ("id_tokens", "secrets")):
            if judged(name):
                r.fail("job %s asks for id_tokens or secrets and a merge request or an unprotected ref can start it" % name)
            else:
                r.note("job %s asks for id_tokens or secrets, on a protected ref only" % name)
    if re.search(r"CI_DEBUG_TRACE\s*:\s*['\"]?true", text, re.I):
        r.fail("CI_DEBUG_TRACE is set: every variable, masked or not, would be printed to a public job log")
    else:
        r.ok("CI_DEBUG_TRACE is set nowhere (the job logs are public)")
    # A value on the key's own line: an id_tokens entry's key holds a mapping (aud), not a
    # value. Each with the job whose block it is in.
    literal = []
    for _, t in sources:
        current = None
        for line in t.splitlines():
            key = top_key(line)
            if key is not None:
                current = key
            m = re.match(r"^[ \t]+([A-Z0-9_]*(?:PASSWORD|TOKEN|SECRET)[A-Z0-9_]*):[ \t]*([^\s#]+)", line)
            if m:
                literal.append((m.group(1), m.group(2), current))
    for key, value, job in literal:
        bare = value.strip("'\"")
        if key.startswith("SECRET_DETECTION_"):
            continue
        if key == "POSTGRES_PASSWORD" and bare == "hmtest":
            r.note("POSTGRES_PASSWORD is written in the file, in %s: the throwaway password of that job's"
                   " own PostgreSQL service container, which nothing outside the job can reach" % job)
        elif key in ("MARIADB_ALLOW_EMPTY_ROOT_PASSWORD", "MARIADB_RANDOM_ROOT_PASSWORD") and bare.lower() in ("1", "yes", "true"):
            r.note("%s is a switch of the MariaDB image, not a credential: no password is written in the file" % key)
        elif bare.startswith("$"):
            r.note("%s is taken from the variable %s, not written in the file" % (key, bare))
        else:
            r.fail("%s is given a value in .gitlab-ci.yml; a credential belongs in a masked, protected variable" % key)
    r.unknown("the project's CI/CD job-token allowlist ('Limit access to this project') and who may"
              " override pipeline variables are not public; a maintainer reads them with"
              " 'glab api projects/%s' (ci_job_token_scope_enabled, ci_pipeline_variables_minimum_override_role)" % PROJECT)
    return r


# ---------------------------------------------------------------- Allstar-Config

# OpenSSF Allstar's repository-level files, the .allstar/ folder that the copy on GitHub
# carries, and the keys each takes: the json tags of config.RepoConfig (allstar.yaml) and
# of each policy package's RepoConfig (pkg/policies/<package>/<package>.go, whose
# configFile names the file) at Allstar's tag v4.6, commit 61e387702. A policy file takes
# optConfig and action beside its own keys. A mapping is a dict, a list of one type is
# [type]. Allstar decodes a file through sigs.k8s.io/yaml and encoding/json, which matches
# a key without regard to case and passes over a key it does not know: a misspelt key is
# a setting silently left at its default, so this holds every key to Allstar's own
# spelling and type.
ALLSTAR_RELEASE = "v4.6"
ALLSTAR_OPT = {"optIn": bool, "optOut": bool}
ALLSTAR_APP_FILE = "allstar.yaml"
ALLSTAR_FILES = {
    ALLSTAR_APP_FILE: {"optConfig": ALLSTAR_OPT, "issueLabel": str, "schedule": {"timezone": str, "days": [str]}},
    "admin.yaml": {"ownerlessAllowed": bool, "userAdminsAllowed": bool, "maxNumberUserAdmins": int,
                   "teamAdminsAllowed": bool, "maxNumberAdminTeams": int},
    "binary_artifacts.yaml": {"ignorePaths": [str]},
    "branch_protection.yaml": {"enforceDefault": bool, "enforceBranches": [str], "requireApproval": bool,
                               "requireCodeOwnerReviews": bool, "approvalCount": int, "dismissStale": bool,
                               "blockForce": bool, "enforceOnAdmins": bool, "requireUpToDateBranch": bool,
                               "requireStatusChecks": [{"context": str, "appID": int}], "requireSignedCommits": bool},
    "codeowners.yaml": {"requireCODEOWNERS": bool},
    "dangerous_workflow.yaml": {"branchList": str},
    "outside.yaml": {"pushAllowed": bool, "adminAllowed": bool},
    "scorecard.yaml": {"checks": [str], "threshold": int, "upload": {"sarif": bool}},
    "security.yaml": {},
}
for _name in ALLSTAR_FILES:
    if _name != ALLSTAR_APP_FILE:
        ALLSTAR_FILES[_name] = dict({"optConfig": ALLSTAR_OPT, "action": str}, **ALLSTAR_FILES[_name])
# The GitHub Actions policy's file: its package has no RepoConfig, and Allstar reads it
# from the organisation-level configuration alone, so in a repository it does nothing.
ALLSTAR_ORG_ONLY = {"actions.yaml"}
# The checks of OpenSSF Scorecard v5.5.0, the release Allstar v4.6 builds with
# (checks.GetAll()); scorecard.yaml's checks name these.
ALLSTAR_SCORECARD_CHECKS = {
    "Binary-Artifacts", "Branch-Protection", "CI-Tests", "CII-Best-Practices", "Code-Review", "Contributors",
    "Dangerous-Workflow", "Dependency-Update-Tool", "Fuzzing", "License", "Maintained", "Packaging",
    "Pinned-Dependencies", "SAST", "Security-Policy", "Signed-Releases", "Token-Permissions", "Vulnerabilities"}
# The plain scalars that the YAML library Allstar uses (go.yaml.in/yaml/v2, under
# sigs.k8s.io/yaml 1.6.0) reads as something other than a string, by its resolve table:
# YAML 1.1's words for true, false and null, the merge key and the special floats. They
# are refused in any case, so that True, YES and .NaN are too; only true and false, in
# lower case, are read, as booleans.
ALLSTAR_SPECIAL = {"y", "yes", "n", "no", "true", "false", "on", "off", "null", "~", "<<",
                   ".nan", ".inf", "+.inf", "-.inf"}
# The one way of writing an integer that this check reads: decimal, no leading zero, no
# plus sign, no underscore. That library reads a plain scalar that begins with a digit,
# a sign or a dot as an integer in any base Go's strconv.ParseInt takes with base 0 (so
# 010 is 8 and 0x1F is 31), with 0b for binary, or as a float, and any other
# such scalar is refused rather than resolved here.
ALLSTAR_INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)")
# Allstar's integers are Go's int, 64 bits on the platforms it is built for.
ALLSTAR_INT_RANGE = (-2 ** 63, 2 ** 63 - 1)
ALLSTAR_KEY = re.compile(r"^([A-Za-z][A-Za-z0-9_]*):(?: (.*))?$")
# What comes before a line's value: its indentation, a list item's dash, and a key.
ALLSTAR_HEAD = re.compile(r" *(?:- +)?(?:[A-Za-z][A-Za-z0-9_]*:(?= |$) *)?")


class AllstarYamlError(ValueError):
    pass


def allstar_scalar(text, where):
    """One scalar of the YAML the .allstar files are written in: true or false, a
    decimal integer, a string in quotes without escapes, or a plain string that YAML
    reads as that string."""
    if text in ("true", "false"):
        return text == "true"
    if ALLSTAR_INTEGER.fullmatch(text):
        # No 64-bit integer is longer than 20 characters, and Python will not convert
        # a string of more than a few thousand digits at all.
        if len(text) > 20 or not ALLSTAR_INT_RANGE[0] <= int(text) <= ALLSTAR_INT_RANGE[1]:
            raise AllstarYamlError("%s: %s does not fit in a 64-bit integer, which Allstar's integers are" % (where, text))
        return int(text)
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "'\"":
        inner = text[1:-1]
        if text[0] in inner or "\\" in inner:
            raise AllstarYamlError("%s: a quoted string with a quote or an escape in it" % where)
        return inner
    if text == "[]":
        return []
    if text == "{}":
        return {}
    if text.lower() in ALLSTAR_SPECIAL:
        raise AllstarYamlError("%s: %r is read by YAML as true, false, null or a special value, not as the plain"
                               " string it looks like; write true, false or a quoted string" % (where, text))
    if text[0] in "0123456789+-.":
        raise AllstarYamlError("%s: %r begins as a YAML number does, and YAML may read it as one (octal, hexadecimal,"
                               " binary or a float); write a decimal integer without a leading zero, or a"
                               " quoted string" % (where, text))
    if text[0] in "[]{}&*!|>%@`'\",#?:" or ": " in text or text.endswith(":"):
        raise AllstarYamlError("%s: %r is YAML this check does not read" % (where, text))
    return text


def allstar_uncomment(raw):
    """raw without its comment, as YAML reads one: a # after a space ends a plain
    value, and a value in quotes ends at its closing quote, whatever follows it."""
    head = ALLSTAR_HEAD.match(raw).group(0)
    value = raw[len(head):]
    if value[:1] in ("'", '"'):
        close = value.find(value[0], 1)
        if close > 0 and re.fullmatch(r"(?: +#.*)?| *", value[close + 1:]):
            value = value[:close + 1]
    elif value.startswith("#") and head.endswith(" "):
        value = ""
    elif " #" in value:
        value = value[:value.index(" #")]
    return (head + value).rstrip(" ")


def allstar_parse(text):
    """The .allstar files' YAML: block mappings and block lists, two or more spaces a
    level, of the scalars allstar_scalar reads, with comments on lines of their own or
    after a value, in printable ASCII (and a byte-order mark before it). Anything else -
    a tab or another control character, a character beyond ASCII, a flow collection other
    than an empty [] or {}, an anchor, a tag, a block scalar, a list in a list, a second
    document, a repeated key - is refused, not guessed."""
    if text.startswith("\ufeff"):
        text = text[1:]
    lines = []
    for number, raw in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
        other = re.search(r"[^\x20-\x7e]", raw)
        if other:
            raise AllstarYamlError("line %d: %s" % (number, "a tab" if other.group(0) == "\t" else
                                                    "the character U+%04X, which is not printable ASCII" % ord(other.group(0))))
        stripped = raw.strip(" ")
        if not stripped or stripped.startswith("#"):
            continue
        if stripped in ("---", "...") or stripped.startswith("%"):
            raise AllstarYamlError("line %d: a document marker or directive" % number)
        body = allstar_uncomment(raw)
        lines.append((number, len(body) - len(body.lstrip(" ")), body.strip(" ")))
    if not lines:
        raise AllstarYamlError("the file holds no setting")

    def block(i, indent):
        number, level, content = lines[i]
        if level != indent:
            raise AllstarYamlError("line %d: indented %d where %d is expected" % (number, level, indent))
        if content.startswith("- ") or content == "-":
            items = []
            while i < len(lines) and lines[i][1] == indent and (lines[i][2].startswith("- ") or lines[i][2] == "-"):
                number, _, content = lines[i]
                rest = content[1:].lstrip(" ")
                if not rest:
                    raise AllstarYamlError("line %d: an empty list item" % number)
                if rest == "-" or rest.startswith("- "):
                    raise AllstarYamlError("line %d: a list in a list" % number)
                if ALLSTAR_KEY.match(rest):
                    # A mapping in a list: its first key on the dash's line, the rest under it.
                    inner = indent + (len(content) - len(rest))
                    lines[i] = (number, inner, rest)
                    value, i = block(i, inner)
                else:
                    value, i = allstar_scalar(rest, "line %d" % number), i + 1
                items.append(value)
            return items, i
        mapping = {}
        while i < len(lines) and lines[i][1] == indent:
            number, _, content = lines[i]
            match = ALLSTAR_KEY.match(content)
            if not match:
                raise AllstarYamlError("line %d: %r is not a key and a value" % (number, content))
            key, value = match.group(1), (match.group(2) or "").strip(" ")
            if key in mapping:
                raise AllstarYamlError("line %d: %s is given twice" % (number, key))
            if value:
                mapping[key], i = allstar_scalar(value, "line %d" % number), i + 1
            elif i + 1 < len(lines) and lines[i + 1][1] > indent:
                mapping[key], i = block(i + 1, lines[i + 1][1])
            elif i + 1 < len(lines) and lines[i + 1][1] == indent and lines[i + 1][2].startswith("-"):
                mapping[key], i = block(i + 1, indent)
            else:
                raise AllstarYamlError("line %d: %s has no value" % (number, key))
        return mapping, i

    value, end = block(0, lines[0][1])
    if end != len(lines):
        number, level, _ = lines[end]
        raise AllstarYamlError("line %d: indented %d, outside the block above it" % (number, level))
    if not isinstance(value, dict):
        raise AllstarYamlError("the file is not a mapping")
    return value


def allstar_typed(value, schema, at):
    """The places where value is not of schema's shape and Allstar's spelling."""
    if isinstance(schema, dict):
        if not isinstance(value, dict):
            return ["%s: a mapping is expected" % (at.rstrip(".") or "the file")]
        wrong = []
        for key in value:
            if key not in schema:
                near = [k for k in schema if k.lower() == key.lower()]
                wrong.append("%s%s: not one of Allstar %s's keys here%s" % (
                    at, key, ALLSTAR_RELEASE, " (it spells it %s)" % near[0] if near else ""))
            else:
                wrong += allstar_typed(value[key], schema[key], at + key + ".")
        return wrong
    if isinstance(schema, list):
        if not isinstance(value, list):
            return ["%s: a list is expected" % at.rstrip(".")]
        wrong = []
        for n, item in enumerate(value):
            wrong += allstar_typed(item, schema[0], "%s[%d]." % (at.rstrip("."), n))
        return wrong
    if schema is int and (isinstance(value, bool) or not isinstance(value, int)):
        return ["%s: an integer is expected" % at.rstrip(".")]
    if schema is not int and not isinstance(value, schema):
        return ["%s: %s is expected" % (at.rstrip("."), "true or false" if schema is bool else "a string")]
    return []


def check_allstar_config(files, reader=None):
    """files: the tracked paths; reader(path) gives a file's text (read, by default)."""
    reader = reader or read
    r = Result("Allstar-Config", "the .allstar/ files of the copy on GitHub are Allstar %s's, every policy"
               " opted in with the action log" % ALLSTAR_RELEASE)
    present = sorted(p[len(".allstar/"):] for p in files if p.startswith(".allstar/"))
    if not present:
        r.fail("no .allstar/ folder: the copy on GitHub is not opted in to Allstar")
        return r
    for name in present:
        if "/" in name:
            r.fail(".allstar/%s: Allstar reads no subfolder of .allstar/" % name)
        elif name in ALLSTAR_ORG_ONLY:
            r.fail(".allstar/%s: Allstar reads the GitHub Actions policy's file from the organisation-level"
                   " configuration alone, so here it would do nothing" % name)
        elif name not in ALLSTAR_FILES:
            r.fail(".allstar/%s: not a file Allstar %s reads in a repository (%s)" % (
                name, ALLSTAR_RELEASE, ", ".join(sorted(ALLSTAR_FILES))))
    for name in sorted(set(ALLSTAR_FILES) - set(present)):
        r.fail(".allstar/%s is missing: %s" % (name, "the repository is not opted in" if name == ALLSTAR_APP_FILE
                                                 else "that policy is not opted in, and an opt-in strategy leaves it off"))
    for name in present:
        if name not in ALLSTAR_FILES:
            continue
        try:
            value = allstar_parse(reader(".allstar/" + name))
        except AllstarYamlError as e:
            r.fail(".allstar/%s: %s" % (name, e))
            continue
        wrong = allstar_typed(value, ALLSTAR_FILES[name], "")
        for w in wrong:
            r.fail(".allstar/%s: %s" % (name, w))
        if wrong:
            continue
        opt = value.get("optConfig", {})
        if opt.get("optIn") is not True or opt.get("optOut") is True:
            r.fail(".allstar/%s: optConfig must have optIn: true and no optOut: true" % name)
        if name != ALLSTAR_APP_FILE and value.get("action") != "log":
            r.fail(".allstar/%s: action must be log - the copy's Issues are off, so Allstar can open no issue,"
                   " and fix would change GitHub's settings, which are the owner's" % name)
        for check in value.get("checks", []) if name == "scorecard.yaml" else []:
            if check not in ALLSTAR_SCORECARD_CHECKS:
                r.fail(".allstar/scorecard.yaml: %s is not one of Scorecard's checks" % check)
        if name == "scorecard.yaml" and not 0 <= value.get("threshold", 10) <= 10:
            r.fail(".allstar/scorecard.yaml: threshold must be from 0 to 10")
        if name == "branch_protection.yaml" and value.get("approvalCount", 1) < 0:
            r.fail(".allstar/branch_protection.yaml: approvalCount cannot be below 0")
        r.ok(".allstar/%s: %s" % (name, json.dumps(value, sort_keys=True, separators=(",", ":"))))
    return r


# ---------------------------------------------------------------- the report

def junit(results):
    cases = []
    for r in results:
        body = escape("\n".join(r.lines))
        name = quoteattr("%s: %s" % (r.name, r.what))
        if r.status == "FAIL":
            cases.append('<testcase classname="repo-hygiene" name=%s><failure message=%s>%s</failure></testcase>' % (
                name, quoteattr("; ".join(l[9:] for l in r.lines if l.startswith("FAIL"))[:500]), body))
        elif r.status == "UNKNOWN":
            cases.append('<testcase classname="repo-hygiene" name=%s><skipped message=%s/><system-out>%s</system-out></testcase>' % (
                name, quoteattr("not answerable from public data"), body))
        else:
            cases.append('<testcase classname="repo-hygiene" name=%s><system-out>%s</system-out></testcase>' % (name, body))
    failures = sum(1 for r in results if r.status == "FAIL")
    skipped = sum(1 for r in results if r.status == "UNKNOWN")
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<testsuites>\n<testsuite name="repo-hygiene" tests="%d" '
            'failures="%d" skipped="%d">\n%s\n</testsuite>\n</testsuites>\n' % (
                len(results), failures, skipped, "\n".join(cases)))


# ---------------------------------------------------------------- the self-test

# default:'s tags, the variable they name and the workflow that chooses it, as
# .gitlab-ci.yml has them (a merge request's pipeline and an unprotected ref's on
# GitLab's hosted runner), and a job that takes them and that every pipeline makes;
# added to SELF_TEST_BASE by the cases below that read them.
RUNNER_TEST_BASE = """
default:
  interruptible: true
  tags: [$HM_RUNNER]

variables:
  HM_RUNNER: linix
  GIT_DEPTH: "1"

workflow:
  rules:
    - if: $CI_PIPELINE_SOURCE == "merge_request_event" && $CI_MERGE_REQUEST_SOURCE_BRANCH_NAME =~ /^batch/
      when: never
    - if: $CI_PIPELINE_SOURCE == "merge_request_event"
      variables:
        HM_RUNNER: saas-linux-small-amd64
    - if: $CI_COMMIT_TAG =~ /^v/ && $CI_COMMIT_REF_PROTECTED == "true"
    - if: $CI_COMMIT_TAG =~ /^v/
      variables:
        HM_RUNNER: saas-linux-small-amd64
    - if: $CI_COMMIT_TAG
      when: never
    - if: $CI_COMMIT_BRANCH && $CI_OPEN_MERGE_REQUESTS && $CI_COMMIT_REF_PROTECTED != "true"
      when: never
    - if: $CI_COMMIT_BRANCH && $CI_COMMIT_REF_PROTECTED == "true"
    - if: $CI_COMMIT_BRANCH
      variables:
        HM_RUNNER: saas-linux-small-amd64
"""
RUNNER_TEST_JOB = """
probe:
  rules:
    - if: $CI_PIPELINE_SOURCE == "schedule"
      when: never
    - when: on_success
  script: [echo]
"""

# A CI file with the project's guard and one job behind it, which Token-Permissions
# passes; each case below adds to it, and says whether the check must then fail.
SELF_TEST_BASE = """\
stages: [test]

.own-runners:
  rules:
    - if: $CI_PIPELINE_SOURCE == "schedule" || $CI_PIPELINE_SOURCE == "pipeline"
      when: never
    - if: $CI_PIPELINE_SOURCE != "merge_request_event" && $CI_COMMIT_REF_PROTECTED == "true"
      changes:
        paths: &not-docs-only
          - "**/*[!d]"

.linix:
  extends: .own-runners
  tags: [linix]

guarded-job:
  extends: .linix
  script:
    - echo guarded
"""

# (name, what is added to the CI file, whether Token-Permissions must fail)
SELF_TEST_CASES = [
    ("a job with rules of its own that ask the same", """
probe:
  extends: .linix
  rules:
    - if: $CI_PIPELINE_SOURCE != "merge_request_event" && $CI_COMMIT_REF_PROTECTED == "true"
  script: [echo]
""", False),
    ("a protected ref and an OR in parentheses beside it", """
probe:
  extends: .linix
  rules:
    - if: $CI_PIPELINE_SOURCE != "merge_request_event" && $CI_COMMIT_REF_PROTECTED == "true" && ($CI_COMMIT_TAG =~ /^v(\\d+)/ || $CI_COMMIT_BRANCH =~ /^batch/)
  script: [echo]
""", False),
    ("the whole condition in parentheses", """
probe:
  extends: .linix
  rules:
    - if: ($CI_PIPELINE_SOURCE != "merge_request_event" && ($CI_COMMIT_REF_PROTECTED=="true"))
  script: [echo]
""", False),
    ("a quoted condition and a comment after it", """
probe:
  extends: .linix
  rules:
    - if: '$CI_PIPELINE_SOURCE != "merge_request_event" && $CI_COMMIT_REF_PROTECTED == "true"'  # quoted
  script: [echo]
""", False),
    ("a condition over two lines", """
probe:
  extends: .linix
  rules:
    - if: $CI_PIPELINE_SOURCE != "merge_request_event" &&
        $CI_COMMIT_REF_PROTECTED == "true"
  script: [echo]
""", False),
    ("merge requests sent to never first, then a protected ref", """
probe:
  tags:
    - docker
    - linix
  rules:
    - if: $CI_PIPELINE_SOURCE == "merge_request_event"
      when: never
    - if: $CI_COMMIT_REF_PROTECTED == "true"
  script: [echo]
""", False),
    ("id_tokens on a protected job, by a template", """
.signing:
  id_tokens:
    PROBE_ID_TOKEN:
      aud: probe
probe:
  extends: [.signing, .linix]
  script: [echo]
""", False),
    ("a protected ref inside an OR", """
probe:
  extends: .linix
  rules:
    - if: $CI_PIPELINE_SOURCE != "merge_request_event" && ($CI_COMMIT_REF_PROTECTED == "true" || $CI_COMMIT_BRANCH =~ /^feature/)
  script: [echo]
""", True),
    ("the merge-request clause inside an OR", """
probe:
  extends: .linix
  rules:
    - if: $CI_COMMIT_REF_PROTECTED == "true" && ($CI_PIPELINE_SOURCE != "merge_request_event" || $CI_COMMIT_BRANCH)
  script: [echo]
""", True),
    ("an OR hidden by parentheses round the whole", """
probe:
  extends: .linix
  rules:
    - if: ($CI_COMMIT_REF_PROTECTED == "true" && $CI_PIPELINE_SOURCE != "merge_request_event" || $CI_COMMIT_BRANCH)
  script: [echo]
""", True),
    ("an OR after a regular expression with a parenthesis in it", """
probe:
  extends: .linix
  rules:
    - if: $CI_COMMIT_REF_PROTECTED == "true" && $CI_PIPELINE_SOURCE != "merge_request_event" && $CI_COMMIT_BRANCH =~ /[(]/ || $CI_COMMIT_BRANCH
  script: [echo]
""", True),
    ("the clauses in a comment", """
probe:
  extends: .linix
  rules:
    - if: $CI_COMMIT_BRANCH # $CI_PIPELINE_SOURCE != "merge_request_event" && $CI_COMMIT_REF_PROTECTED == "true"
  script: [echo]
""", True),
    ("an OR on the condition's second line", """
probe:
  extends: .linix
  rules:
    - if: $CI_PIPELINE_SOURCE != "merge_request_event" && $CI_COMMIT_REF_PROTECTED == "true"
        || $CI_COMMIT_BRANCH
  script: [echo]
""", True),
    ("a rules list at the key's own indentation", """
probe:
  extends: .linix
  rules:
  - if: $CI_COMMIT_BRANCH
  script: [echo]
""", True),
    ("a rules list indented six", """
probe:
  extends: .linix
  rules:
      - if: $CI_COMMIT_BRANCH
  script: [echo]
""", True),
    ("rules written inline, over a guarded parent", """
probe:
  extends: .linix
  rules: [{if: $CI_COMMIT_BRANCH}]
  script: [echo]
""", True),
    ("rules by an alias, over a guarded parent", """
probe:
  extends: .linix
  rules: *probe-rules
  script: [echo]
""", True),
    ("a merge key", """
probe:
  <<: *probe-job
  extends: .linix
  script: [echo]
""", True),
    ("rules from the last of two parents, which asks the same", """
.probe-rules:
  rules:
    - if: $CI_PIPELINE_SOURCE != "merge_request_event" && $CI_COMMIT_REF_PROTECTED == "true"
probe:
  extends: [.linix, .probe-rules]
  script: [echo]
""", False),
    ("rules from the last of two parents, over the guarded first", """
.probe-rules:
  rules:
    - if: $CI_COMMIT_BRANCH
probe:
  extends: [.linix, .probe-rules]
  script: [echo]
""", True),
    ("extends as a block list", """
probe:
  extends:
    - .linix
  rules:
    - if: $CI_COMMIT_BRANCH
  script: [echo]
""", True),
    ("extends by an alias, the tags only in what it names", """
probe:
  extends: *probe-parent
  rules:
    - if: $CI_COMMIT_BRANCH
  script: [echo]
""", True),
    ("the project's runner second in a block list of tags", """
probe:
  tags:
    - docker
    - linix
  rules:
    - if: $CI_COMMIT_BRANCH
  script: [echo]
""", True),
    ("a tag of linix's other than its name", """
probe:
  tags: [docker]
  rules:
    - if: $CI_COMMIT_BRANCH
  script: [echo]
""", True),
    ("tags from a variable", """
probe:
  tags: [$PROBE_RUNNER]
  rules:
    - if: $CI_COMMIT_BRANCH
  script: [echo]
""", True),
    ("merge requests sent to never only when a file changed", """
probe:
  tags: [linix]
  rules:
    - if: $CI_PIPELINE_SOURCE == "merge_request_event"
      changes: [README.md]
      when: never
    - if: $CI_COMMIT_REF_PROTECTED == "true"
  script: [echo]
""", True),
    ("id_tokens by a template, on a job anyone can start", """
.signing:
  id_tokens:
    PROBE_ID_TOKEN:
      aud: probe
probe:
  extends: .signing
  script: [echo]
""", True),
    ("the job token in a template's script, by an anchored key", """
.probe-template: &probe
  script:
    - echo "$CI_JOB_TOKEN"
probe:
  extends: .probe-template
""", True),
    ("the job token in a script a template runs", """
.probe-template:
  script:
    - bash build/ci/probe-credential.sh
probe:
  extends: .probe-template
""", True),
    ("an include this check cannot read", """
include:
  - remote: https://example.invalid/probe.yml
""", True),
    ("an include of a file that is not there", """
include:
  - local: .gitlab/ci/not-there.yml
""", True),
    ("an !reference the files do not resolve", """
probe:
  extends: .linix
  rules:
    - !reference [.not-there, rules]
  script: [echo]
""", True),
    ("default:'s runner, which the workflow gives GitLab's for a merge request and an unprotected ref",
     RUNNER_TEST_BASE + RUNNER_TEST_JOB, False),
    ("default:'s runner, the workflow leaving a merge request on the project's",
     RUNNER_TEST_BASE.replace('    - if: $CI_PIPELINE_SOURCE == "merge_request_event"\n      variables:\n'
                              '        HM_RUNNER: saas-linux-small-amd64\n',
                              '    - if: $CI_PIPELINE_SOURCE == "merge_request_event"\n') + RUNNER_TEST_JOB, True),
    ("default:'s runner, the workflow leaving an unprotected branch on the project's",
     RUNNER_TEST_BASE.replace("    - if: $CI_COMMIT_BRANCH\n      variables:\n        HM_RUNNER: saas-linux-small-amd64\n",
                              "    - if: $CI_COMMIT_BRANCH\n") + RUNNER_TEST_JOB, True),
    ("default:'s runner, the workflow's protected branch rule asking nothing of the ref",
     RUNNER_TEST_BASE.replace('    - if: $CI_COMMIT_BRANCH && $CI_COMMIT_REF_PROTECTED == "true"\n',
                              "    - if: $CI_COMMIT_BRANCH\n") + RUNNER_TEST_JOB, True),
    ("default:'s runner, a job that sets it to linix itself",
     RUNNER_TEST_BASE + RUNNER_TEST_JOB + "  variables:\n    HM_RUNNER: linix\n", True),
    ("default: naming linix itself, a job that any pipeline makes",
     RUNNER_TEST_BASE.replace("  tags: [$HM_RUNNER]\n", "  tags: [linix]\n") + RUNNER_TEST_JOB, True),
    ("default:'s tags as an alias", RUNNER_TEST_BASE.replace("  tags: [$HM_RUNNER]\n", "  tags: *probe-tags\n")
     + RUNNER_TEST_JOB, True),
]


# Pinned-Dependencies' CI half: a .gitlab-ci.yml that includes a file of its own, with
# every image pinned; each case puts something into the included file, and says
# whether the check must then fail.
PINNED_TEST_BASE = """\
include:
  - local: .gitlab/ci/included.yml

top-job:
  image: alpine:3.24@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6
  script: [echo]
"""
PINNED_TEST_INCLUDED = """\
included-job:
  image:
    name: python:3.14-slim-trixie@sha256:51dafde81dbdb6ebde285137a295cf18a47ca95234fe388a343719cb97305b3d
  services:
    - name: postgres:16@sha256:1a6ab3f5345eb6dbe04a1349529caabdb0ab09293a09590fad07b2246bfa4b54
      alias: postgres
  script:
    - pip install --require-hashes -r requirements.txt
"""
PINNED_TEST_CASES = [
    ("an included job's image by tag", ("python:3.14-slim-trixie@sha256:51dafde81dbdb6ebde285137a295cf18a47ca95234fe388a343719cb97305b3d",
                                         "python:3.14-slim-trixie"), True),
    ("an included job's service by tag", ("postgres:16@sha256:1a6ab3f5345eb6dbe04a1349529caabdb0ab09293a09590fad07b2246bfa4b54",
                                           "postgres:16"), True),
    ("an unhashed pip install in an included job", ("pip install --require-hashes -r", "pip install -r"), True),
    ("a local wheel installed with --no-index", ("pip install --require-hashes -r requirements.txt",
                                                 "pip install --no-index dist/hmailserver_cli-9.9.9-py3-none-any.whl"), False),
]


def self_test_pinned():
    """Pinned-Dependencies' CI half against PINNED_TEST_BASE and each case; the number
    judged wrongly."""
    global ROOT
    saved, wrong = ROOT, 0
    try:
        for name, (old, new), must_fail in [("the pinned base and its include", ("", ""), False)] + PINNED_TEST_CASES:
            scratch = tempfile.mkdtemp(prefix="repo-hygiene-self-test-")
            try:
                ROOT = scratch
                os.makedirs(os.path.join(scratch, ".gitlab", "ci"))
                with open(os.path.join(scratch, ".gitlab-ci.yml"), "w", encoding="utf-8", newline="\n") as f:
                    f.write(PINNED_TEST_BASE)
                included = PINNED_TEST_INCLUDED.replace(old, new, 1) if old else PINNED_TEST_INCLUDED
                if old and included == PINNED_TEST_INCLUDED:
                    raise AssertionError("the self-test's case %r changes nothing" % name)
                with open(os.path.join(scratch, ".gitlab", "ci", "included.yml"), "w", encoding="utf-8", newline="\n") as f:
                    f.write(included)
                result = check_pinned_dependencies([".gitlab-ci.yml", ".gitlab/ci/included.yml"])
            finally:
                ROOT = saved
                shutil.rmtree(scratch, ignore_errors=True)
            failed = result.status == "FAIL"
            right = failed == must_fail
            wrong += 0 if right else 1
            print("%-6s Pinned-Dependencies, %s: %s, as %s" % (
                "ok" if right else "WRONG", name, result.status,
                "it must" if right else ("it must fail" if must_fail else "it must not fail")))
            if not right or failed:
                for line in result.lines:
                    if line.startswith("FAIL"):
                        print("         " + line[9:][:300])
        # A workflow under .github/workflows/ (the copy on GitHub's), beside the pinned
        # base: an action pinned by a full commit passes, one named by a tag fails.
        for name, uses, must_fail in PINNED_WORKFLOW_CASES:
            scratch = tempfile.mkdtemp(prefix="repo-hygiene-self-test-")
            try:
                ROOT = scratch
                os.makedirs(os.path.join(scratch, ".gitlab", "ci"))
                os.makedirs(os.path.join(scratch, ".github", "workflows"))
                with open(os.path.join(scratch, ".gitlab-ci.yml"), "w", encoding="utf-8", newline="\n") as f:
                    f.write(PINNED_TEST_BASE)
                with open(os.path.join(scratch, ".gitlab", "ci", "included.yml"), "w", encoding="utf-8", newline="\n") as f:
                    f.write(PINNED_TEST_INCLUDED)
                with open(os.path.join(scratch, ".github", "workflows", "w.yml"), "w", encoding="utf-8", newline="\n") as f:
                    f.write("jobs:\n  a:\n    runs-on: ubuntu-latest\n    steps:\n      - uses: ./local\n"
                            "      - uses: %s\n" % uses)
                result = check_pinned_dependencies([".gitlab-ci.yml", ".gitlab/ci/included.yml", ".github/workflows/w.yml"])
            finally:
                ROOT = saved
                shutil.rmtree(scratch, ignore_errors=True)
            failed = result.status == "FAIL"
            right = failed == must_fail
            wrong += 0 if right else 1
            print("%-6s Pinned-Dependencies, %s: %s, as %s" % (
                "ok" if right else "WRONG", name, result.status,
                "it must" if right else ("it must fail" if must_fail else "it must not fail")))
            if not right or failed:
                for line in result.lines:
                    if line.startswith("FAIL"):
                        print("         " + line[9:][:300])
    finally:
        ROOT = saved
    return wrong


# Pinned-Dependencies over a workflow of the copy on GitHub: (name, the second step's
# uses:, whether the check must fail). The first step, a local action, is never asked
# for a commit.
PINNED_WORKFLOW_CASES = [
    ("a GitHub workflow's action pinned by a full commit",
     "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1", False),
    ("a GitHub workflow's action named by a tag that can move", "actions/checkout@v4", True),
]


# Allstar-Config's self-test: a folder that passes, and each case's change to it - in a
# file, (file, old, new) with old replaced by new once; a file added, (file, None, text);
# a file taken away, (file, None, None) - and, for a case the check must fail, words its
# failure must hold, so that each case is failed by the rule it is named for (None for a
# case it must pass).
ALLSTAR_TEST_BASE = dict({name: "optConfig:\n  optIn: true\naction: log\n" for name in ALLSTAR_FILES}, **{
    "allstar.yaml": "# The repository opts in.\noptConfig:\n  optIn: true\n",
    "branch_protection.yaml": ("optConfig:\n  optIn: true\naction: log  # never an issue\napprovalCount: 2\n"
                               "requireCodeOwnerReviews: true\nrequireStatusChecks:\n  - context: CodeQL\n"
                               "    appID: 15368\nenforceOnAdmins: false\n"),
    "scorecard.yaml": "optConfig:\n  optIn: true\naction: log\nchecks:\n- SAST\n- 'License'\nthreshold: 10\nupload:\n  sarif: false\n",
})
ALLSTAR_TEST_CASES = [
    ("a key Allstar does not have", ("branch_protection.yaml", "approvalCount", "approvalsCount"),
     "approvalsCount: not one of Allstar"),
    ("a key in another case than Allstar's", ("branch_protection.yaml", "approvalCount", "approvalcount"),
     "(it spells it approvalCount)"),
    ("an integer written as a word", ("branch_protection.yaml", "approvalCount: 2", "approvalCount: two"),
     "approvalCount: an integer is expected"),
    ("true or false where an integer is expected", ("branch_protection.yaml", "approvalCount: 2", "approvalCount: true"),
     "approvalCount: an integer is expected"),
    ("a string where true or false is expected", ("branch_protection.yaml", "requireCodeOwnerReviews: true",
                                                  "requireCodeOwnerReviews: CodeQL"),
     "requireCodeOwnerReviews: true or false is expected"),
    ("an integer where true or false is expected", ("branch_protection.yaml", "requireCodeOwnerReviews: true",
                                                    "requireCodeOwnerReviews: 1"),
     "requireCodeOwnerReviews: true or false is expected"),
    ("an integer where a string is expected", ("branch_protection.yaml", "- context: CodeQL", "- context: 5"),
     "requireStatusChecks[0].context: a string is expected"),
    ("true where a string is expected", ("branch_protection.yaml", "- context: CodeQL", "- context: true"),
     "requireStatusChecks[0].context: a string is expected"),
    ("a value where a list is expected", ("branch_protection.yaml", "enforceOnAdmins: false",
                                          "enforceOnAdmins: false\nenforceBranches: master"),
     "enforceBranches: a list is expected"),
    ("a value where a mapping is expected", ("allstar.yaml", "optConfig:\n  optIn: true", "optConfig: true"),
     "optConfig: a mapping is expected"),
    ("YAML 1.1's yes for true", ("branch_protection.yaml", "requireCodeOwnerReviews: true",
                                 "requireCodeOwnerReviews: yes"), "is read by YAML as true, false, null"),
    ("True, in another case than true", ("branch_protection.yaml", "requireCodeOwnerReviews: true",
                                         "requireCodeOwnerReviews: True"), "is read by YAML as true, false, null"),
    ("YAML 1.1's on where a string is expected", ("branch_protection.yaml", "- context: CodeQL", "- context: on"),
     "is read by YAML as true, false, null"),
    ("a special float", ("dangerous_workflow.yaml", "action: log", "action: log\nbranchList: .NaN"),
     "is read by YAML as true, false, null"),
    ("an integer with a leading zero, which YAML reads as octal", ("scorecard.yaml", "threshold: 10", "threshold: 010"),
     "begins as a YAML number does"),
    ("a hexadecimal number where a string is expected", ("branch_protection.yaml", "- context: CodeQL",
                                                         "- context: 0x1F"), "begins as a YAML number does"),
    ("a binary number where a string is expected", ("branch_protection.yaml", "- context: CodeQL",
                                                    "- context: 0b101"), "begins as a YAML number does"),
    ("an integer beyond 64 bits", ("branch_protection.yaml", "approvalCount: 2", "approvalCount: 99999999999999999999"),
     "does not fit in a 64-bit integer"),
    ("a status check named by name: rather than context:", ("branch_protection.yaml", "- context:", "- name:"),
     "name: not one of Allstar"),
    ("a status check's appID as a string", ("branch_protection.yaml", "appID: 15368", "appID: '15368'"),
     "appID: an integer is expected"),
    ("the action issue", ("security.yaml", "action: log", "action: issue"), "action must be log"),
    ("the action fix", ("outside.yaml", "action: log", "action: fix"), "action must be log"),
    ("a policy not opted in", ("admin.yaml", "optIn: true", "optIn: false"), "optConfig must have optIn: true"),
    ("the repository opted out", ("allstar.yaml", "optIn: true", "optIn: true\n  optOut: true"),
     "optConfig must have optIn: true"),
    ("an organisation-level key in the repository's allstar.yaml", ("allstar.yaml", "optIn: true", "optOutStrategy: true"),
     "optOutStrategy: not one of Allstar"),
    ("an organisation-level key in a policy file", ("outside.yaml", "action: log", "action: log\nexemptions: []"),
     "exemptions: not one of Allstar"),
    ("a Scorecard check that does not exist", ("scorecard.yaml", "- SAST", "- Signed-Release"),
     "Signed-Release is not one of Scorecard's checks"),
    ("a threshold above 10", ("scorecard.yaml", "threshold: 10", "threshold: 11"), "threshold must be from 0 to 10"),
    ("a threshold below 0", ("scorecard.yaml", "threshold: 10", "threshold: -1"), "threshold must be from 0 to 10"),
    ("an approval count below 0", ("branch_protection.yaml", "approvalCount: 2", "approvalCount: -1"),
     "approvalCount cannot be below 0"),
    ("a file Allstar does not read", ("notapolicy.yaml", None, "action: log\n"), "not a file Allstar"),
    ("a file in a subfolder", ("sub/security.yaml", None, "optConfig:\n  optIn: true\naction: log\n"),
     "Allstar reads no subfolder"),
    ("actions.yaml, which Allstar reads at the organisation level alone", ("actions.yaml", None, "action: log\ngroups: []\n"),
     "from the organisation-level configuration alone"),
    ("a policy's file taken away", ("security.yaml", None, None), "security.yaml is missing"),
    ("allstar.yaml taken away", ("allstar.yaml", None, None), "the repository is not opted in"),
    ("every file taken away", ("*", None, None), "no .allstar/ folder"),
    ("a tab in the indentation", ("codeowners.yaml", "  optIn", "  \toptIn"), "a tab"),
    ("a control character after a string", ("branch_protection.yaml", "- context: CodeQL", "- context: CodeQL\x0c"),
     "U+000C"),
    ("a no-break space after true", ("security.yaml", "optIn: true", "optIn: true\xa0"), "U+00A0"),
    ("a document marker", ("codeowners.yaml", "optConfig:", "---\noptConfig:"), "a document marker"),
    ("a flow sequence where a string is expected", ("branch_protection.yaml", "- context: CodeQL",
                                                    "- context: [CodeQL]"), "'[CodeQL]' is YAML this check does not read"),
    ("a key and a value where a string is expected", ("branch_protection.yaml", "- context: CodeQL",
                                                      "- context: CodeQL: x"), "'CodeQL: x' is YAML this check does not read"),
    ("a quote inside a quoted string", ("scorecard.yaml", "- 'License'", "- 'Lic'ense'"),
     "a quoted string with a quote or an escape"),
    ("an escape inside a quoted string", ("scorecard.yaml", "- 'License'", '- "Lic\\x65nse"'),
     "a quoted string with a quote or an escape"),
    ("a list in a list", ("allstar.yaml", "optIn: true", "optIn: true\nschedule:\n  days:\n  - - saturday"),
     "a list in a list"),
    ("a line indented past its block", ("dangerous_workflow.yaml", "action: log", "action: log\n  branchList: master"),
     "outside the block above it"),
    ("a list item's dash and three spaces, its next key under its first", (
        "branch_protection.yaml", "  - context: CodeQL\n    appID: 15368", "  -   context: CodeQL\n      appID: 15368"), None),
    ("a list item's dash and three spaces, its next key two columns past the dash", (
        "branch_protection.yaml", "  - context: CodeQL\n    appID: 15368", "  -   context: CodeQL\n    appID: 15368"),
     "outside the block above it"),
    ("a list where the file's mapping should be", ("security.yaml", "optConfig:\n  optIn: true\naction: log\n",
                                                   "- optIn\n"), "the file is not a mapping"),
    ("a key given twice", ("dangerous_workflow.yaml", "action: log", "action: log\naction: log"), "action is given twice"),
    ("an empty file", ("security.yaml", "optConfig:\n  optIn: true\naction: log\n", "# nothing\n"),
     "the file holds no setting"),
    ("a comment after a value and a string in quotes", ("dangerous_workflow.yaml", "action: log",
                                                         "action: \"log\"\nbranchList: default  # the default branch"), None),
    ("a comment with a quote in it", ("branch_protection.yaml", "# never an issue", "# Allstar's default is issue"), None),
    ("strings made of a number's letters", ("branch_protection.yaml", "- context: CodeQL",
                                            "- context: e2e\n  - context: exe"), None),
    ("a byte-order mark", ("security.yaml", "optConfig:", chr(0xFEFF) + "optConfig:"), None),
]
# allstar_parse's own self-test: a file's text and the value it must read, or words of
# the refusal it must give.
ALLSTAR_PARSE_CASES = [
    ("a comment after a plain value", "a: log  # it's log\n", {"a": "log"}),
    ("a # with no space before it, which begins no comment", "a: C#\n", {"a": "C#"}),
    ("a # inside a quoted string", "a: 'x # y'  # z\n", {"a": "x # y"}),
    ("a comment where a value would be", "a: # the block below\n  b: 1\n", {"a": {"b": 1}}),
    ("a negative integer and 0", "a: -1\nb: 0\n", {"a": -1, "b": 0}),
    ("the largest 64-bit integer", "a: 9223372036854775807\n", {"a": 9223372036854775807}),
    ("a byte-order mark and CRLF", chr(0xFEFF) + "a: 1\r\nb:\r\n  - e2e\r\n", {"a": 1, "b": ["e2e"]}),
    ("a mapping in a list", "a:\n- b: 1\n  c: exe\n", {"a": [{"b": 1, "c": "exe"}]}),
    ("an empty list and an empty mapping", "a: []\nb: {}\n", {"a": [], "b": {}}),
    ("a lone carriage return, which YAML reads as a line break", "a: 1\rb: 2\n", "U+000D"),
    ("a character beyond ASCII", "a: caf" + chr(0xE9) + "\n", "U+00E9"),
    ("an em space before a #", "a: log" + chr(0x2003) + "# the action\n", "U+2003"),
    ("YAML's null", "a: ~\n", "is read by YAML as true, false, null"),
    ("an integer one past 64 bits", "a: 9223372036854775808\n", "does not fit in a 64-bit integer"),
    ("a date, which this check does not read", "a: 2026-10-24\n", "begins as a YAML number does"),
    ("an integer with a plus sign", "a: +1\n", "begins as a YAML number does"),
    ("an integer with an underscore", "a: 1_000\n", "begins as a YAML number does"),
    ("an empty list item", "a:\n-\n", "an empty list item"),
    ("a key with no value", "a:\n", "a has no value"),
    ("a tag", "a: !!str x\n", "is YAML this check does not read"),
    ("a block scalar", "a: |\n  x\n", "is YAML this check does not read"),
    ("an anchor", "a: &x b\n", "is YAML this check does not read"),
    ("a value after a closing quote", "a: 'x' y\n", "is YAML this check does not read"),
    ("a value that ends in a colon", "a: b:\n", "is YAML this check does not read"),
    ("a line with no key", "a: 1\nb\n", "is not a key and a value"),
    ("a # inside a quoted list item", "a:\n- 'x # y'  # z\n", {"a": ["x # y"]}),
    ("a list item with a colon in it, not a key", "a:\n- C:x\n", {"a": ["C:x"]}),
    ("the smallest 64-bit integer", "a: -9223372036854775808\n", {"a": -9223372036854775808}),
    ("an integer one below 64 bits", "a: -9223372036854775809\n", "does not fit in a 64-bit integer"),
    ("an integer of 4,301 digits, more than Python converts", "a: " + "9" * 4301 + "\n",
     "does not fit in a 64-bit integer"),
    ("two byte-order marks", chr(0xFEFF) + chr(0xFEFF) + "a: 1\n", "U+FEFF"),
]
# allstar_scalar's own self-test: each word, first character and form its rules refuse,
# with words of the refusal it must give. They are written out here, not drawn from the
# rules' own tables, so that a word or a character taken away from a rule fails a case.
ALLSTAR_REFUSED_SCALARS = (
    [(word, "is read by YAML as true, false, null") for word in (
        "y", "Y", "yes", "Yes", "n", "N", "no", "NO", "on", "On", "off", "Off", "null", "Null", "NULL", "~",
        "True", "TRUE", "False", "FALSE", "<<", ".nan", ".NaN", ".inf", "+.inf", "-.Inf")]
    + [(text, "begins as a YAML number does") for text in (
        "0.5", "1e3", "2.5", "3.5", "4e4", "5.0", "6e1", "7.25", "8e8", "9.9", ".5", "-0x1F", "-.5", "+1")]
    + [(text, "is YAML this check does not read") for text in (
        "[x", "]x", "{x}", "}x", "&x", "*x", "!x", "|", ">", "%x", "@x", "`x", "'x'y", '"x"y', ",x", "#x", "?x", ":x",
        "x: y", "x:")]
)


def self_test_allstar():
    """allstar_parse over ALLSTAR_PARSE_CASES, then Allstar-Config over the tree's own
    .allstar/ and over ALLSTAR_TEST_BASE and each of ALLSTAR_TEST_CASES; the number
    judged wrongly."""
    wrong = 0
    for name, text, expected in ALLSTAR_PARSE_CASES:
        try:
            got = json.dumps(allstar_parse(text), sort_keys=True)
        except AllstarYamlError as e:
            got = "refused: %s" % e
        right = (expected in got and got.startswith("refused: ") if isinstance(expected, str)
                 else got == json.dumps(expected, sort_keys=True))
        wrong += 0 if right else 1
        print("%-6s Allstar-Config's YAML, %s: %s" % ("ok" if right else "WRONG", name, got[:200]))
    for text, expected in ALLSTAR_REFUSED_SCALARS:
        try:
            got = json.dumps(allstar_scalar(text, "the value"))
        except AllstarYamlError as e:
            got = "refused: %s" % e
        right = got.startswith("refused: ") and expected in got
        wrong += 0 if right else 1
        print("%-6s Allstar-Config's YAML, the plain value %r: %s" % ("ok" if right else "WRONG", text, got[:200]))
    in_tree = os.path.join(ROOT, ".allstar")
    tree = sorted(".allstar/" + n for n in os.listdir(in_tree)) if os.path.isdir(in_tree) else []
    result = check_allstar_config(tree)
    right = result.status == "PASS"
    wrong += 0 if right else 1
    print("%-6s Allstar-Config, the tree's own .allstar/ (%d files): %s, as it must not fail" % (
        "ok" if right else "WRONG", len(tree), result.status))
    for line in result.lines if not right else []:
        print("         " + line[:300])
    for name, change, expect in [("the passing folder", None, None)] + ALLSTAR_TEST_CASES:
        must_fail = expect is not None
        folder = dict(ALLSTAR_TEST_BASE)
        if change:
            target, old, new = change
            if target == "*":
                folder = {}
            elif old is None and new is None:
                del folder[target]
            elif old is None:
                folder[target] = new
            else:
                if old not in folder[target]:
                    raise AssertionError("the self-test's case %r changes nothing" % name)
                folder[target] = folder[target].replace(old, new, 1)
        result = check_allstar_config([".allstar/" + file for file in folder],
                                      lambda path, folder=folder: folder[path[len(".allstar/"):]])
        failed = result.status == "FAIL"
        said = not must_fail or any(line.startswith("FAIL") and expect in line for line in result.lines)
        right = failed == must_fail and said
        wrong += 0 if right else 1
        print("%-6s Allstar-Config, %s: %s, as %s" % (
            "ok" if right else "WRONG", name, result.status,
            "it must" if right else ("it must fail" if must_fail and not failed else "it must not fail" if not must_fail
                                     else "it must fail saying %r" % expect)))
        if not right or failed:
            for line in result.lines:
                if line.startswith("FAIL"):
                    print("         " + line[9:][:300])
    return wrong


def self_test():
    """Token-Permissions against SELF_TEST_BASE and each case: the base and the
    cases that must pass are not FAILed, and every other case is. Then
    Pinned-Dependencies' CI half (self_test_pinned), and Allstar-Config
    (self_test_allstar)."""
    global ROOT
    saved, wrong = ROOT, 0
    try:
        for name, addition, must_fail in [("the base itself", "", False)] + SELF_TEST_CASES:
            scratch = tempfile.mkdtemp(prefix="repo-hygiene-self-test-")
            try:
                ROOT = scratch
                os.makedirs(os.path.join(scratch, "build", "ci"))
                with open(os.path.join(scratch, "build", "ci", "probe-credential.sh"), "w", encoding="utf-8", newline="\n") as f:
                    f.write('#!/bin/sh\ncurl --header "JOB-TOKEN: $CI_JOB_TOKEN" https://example.invalid/\n')
                with open(os.path.join(scratch, ".gitlab-ci.yml"), "w", encoding="utf-8", newline="\n") as f:
                    f.write(SELF_TEST_BASE + addition)
                result = check_token_permissions(["build/ci/probe-credential.sh"] if "probe-credential" in addition else [])
            finally:
                ROOT = saved
                shutil.rmtree(scratch, ignore_errors=True)
            failed = result.status == "FAIL"
            right = failed == must_fail
            wrong += 0 if right else 1
            print("%-6s %s: %s, as %s" % ("ok" if right else "WRONG", name, result.status,
                                         "it must" if right else ("it must fail" if must_fail else "it must not fail")))
            if not right or failed:
                for line in result.lines:
                    if line.startswith("FAIL"):
                        print("         " + line[9:][:300])
    finally:
        ROOT = saved
    wrong += self_test_pinned()
    # Signed-Releases' reading of a release's assets: 6.3.6's provenance is provenance.
    six = ["hmailserver_6.3.6_amd64.deb", "hmailserver_6.3.6_amd64.deb.cosign.bundle", "hmailserver_6.3.6_amd64.deb.sigstore.json",
           "hmailserver-6.3.6.intoto.sigstore.json"]
    for name, names, signatures, provenance in (
            ("6.3.6's assets", six, 3, ["hmailserver-6.3.6.intoto.sigstore.json"]),
            ("6.3.3's assets", ["hMailServer-6.3.3.exe", "hMailServer-6.3.3.exe.sigstore.json", "hmailserver-v6.3.3.intoto.jsonl"],
             1, ["hmailserver-v6.3.3.intoto.jsonl"]),
            ("6.3.5's assets", ["hmailserver_6.3.5_amd64.deb", "hmailserver_6.3.5_amd64.deb.cosign.bundle"], 1, [])):
        got = release_assets(names)
        right = len(got[0]) == signatures and got[1] == provenance
        wrong += 0 if right else 1
        print("%-6s Signed-Releases, %s: %d signature(s), provenance %s" % ("ok" if right else "WRONG", name, len(got[0]), got[1]))
    wrong += self_test_allstar()
    print("%d case(s), %d judged wrongly" % (len(SELF_TEST_CASES) + 1 + len(PINNED_TEST_CASES) + 1 + len(PINNED_WORKFLOW_CASES) + 3
                                             + len(ALLSTAR_PARSE_CASES) + len(ALLSTAR_REFUSED_SCALARS) + 2
                                             + len(ALLSTAR_TEST_CASES), wrong))
    return 1 if wrong else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", default=os.path.join("security", "repo-hygiene"))
    parser.add_argument("--self-test", action="store_true",
                        help="prove Token-Permissions and Pinned-Dependencies' CI half against CI files made to "
                             "break them, and Allstar-Config against the tree's .allstar/ and folders made to "
                             "break it, offline, and exit")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    os.chdir(ROOT)
    files = tracked_files()
    results = [
        check_branch_protection(),
        check_signed_releases(),
        check_security_policy(),
        check_pinned_dependencies(files),
        check_binary_artifacts(files),
        check_token_permissions(files),
        check_allstar_config(files),
    ]
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "repo-hygiene.json"), "w", encoding="utf-8") as f:
        json.dump([{"check": r.name, "what": r.what, "status": r.status, "details": r.lines} for r in results], f, indent=1)
    with open(os.path.join(args.out, "junit.xml"), "w", encoding="utf-8") as f:
        f.write(junit(results))
    print("Repository hygiene of project %s, read anonymously from %s" % (PROJECT, API))
    for r in results:
        print("")
        print("%-8s %s - %s" % (r.status, r.name, r.what))
        for line in r.lines:
            print("   " + line)
    print("")
    print("  ".join("%s %s" % (r.name, r.status) for r in results))
    return 1 if any(r.status == "FAIL" for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
