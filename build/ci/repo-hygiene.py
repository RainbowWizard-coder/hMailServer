#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The repository-hygiene checks OpenSSF Scorecard ran, answered from GitLab.

Scorecard (the GitHub workflow scorecard.yml) stopped with the GitHub account on 18
September 2026, and it reads GitHub's API only. This is the part of it that GitLab
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
  Pinned-Dependencies  every image in .gitlab-ci.yml and every FROM in a Dockerfile
                       pinned by digest (or, for the CI image, by the tag made from
                       its Dockerfile's hash that build/check-ci-image.py enforces);
                       no pip install without hashes, npm install, or piped curl in
                       a CI script; a NuGet lock file beside every project that
                       has package references.
  Binary-Artifacts     no executable or compiled file tracked in git (by content,
                       not only by extension).
  Token-Permissions    no job that a merge request or an unprotected ref can start
                       reaches the project's own runners, asks for id_tokens or
                       secrets, or runs a CI script that handles CI_JOB_TOKEN or
                       another credential - judged by the rules each job runs by,
                       its own or those it extends, in .gitlab-ci.yml and the
                       files it includes; no credential written in those files;
                       CI_DEBUG_TRACE (which prints every variable into a public
                       job log) set nowhere. The project's job-token allowlist and
                       pipeline-variable settings are not public, and the check
                       says so rather than guessing.

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
an include it cannot open - and fails if any is judged wrongly.

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
REPOSITORY = os.environ.get("CI_PROJECT_URL", "https://gitlab.com/Progressiverobot/hmailserver") + ".git"

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
PROVENANCE_SUFFIXES = (".intoto.jsonl",)


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
        signatures = [n for n in names if n.endswith(SIGNATURE_SUFFIXES)]
        provenance = [n for n in names if n.endswith(PROVENANCE_SUFFIXES)]
        with_provenance += 1 if provenance else 0
        if signatures:
            r.ok("%s: %d signature asset(s) for %d asset(s)%s" % (
                rel["tag_name"], len(signatures), len(names),
                ", and build provenance (%s)" % provenance[0] if provenance else ""))
        else:
            r.fail("%s: no signature asset among its %d asset(s)" % (rel["tag_name"], len(names)))
    r.note("%d of %d carry build provenance (in-toto); Scorecard scored provenance above signatures alone" % (
        with_provenance, len(newest)))
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
    ci = read(".gitlab-ci.yml") if os.path.exists(os.path.join(ROOT, ".gitlab-ci.yml")) else ""
    unpinned = []
    images = ci_images(ci)
    for line, image in images:
        if "@sha256:" in image:
            continue
        if CI_IMAGE_TAG.match(image):
            continue
        if image.startswith("$"):
            continue
        unpinned.append("%s (.gitlab-ci.yml line %d)" % (image, line))
    if unpinned:
        r.fail("%d image(s) in .gitlab-ci.yml named by a tag that can move: %s" % (len(unpinned), "; ".join(unpinned)))
    else:
        r.ok("all %d image(s) in .gitlab-ci.yml pinned by digest, or the CI image by its Dockerfile's hash" % len(images))

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

    # No GitHub workflow is left to check: .github/workflows left the tree on 26
    # September 2026 (they are in the history, at v6.3.3 for instance), so the
    # check that their actions were pinned by commit is retired with them.

    scripts = [f for f in files if f == ".gitlab-ci.yml" or re.match(r"^build/(ci/)?[^/]+\.sh$", f)
               or (f.startswith("build/ci/") and f.endswith(".Dockerfile")) or f in dockerfiles]
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
            if re.search(r"\bpip3?[\"']?\s+install\b", code) and "--require-hashes" not in code:
                risky.append("%s line %d: pip install without --require-hashes" % (path, i + 1))
            if re.search(r"\bnpm\s+(install|i)\b", code):
                risky.append("%s line %d: npm install (npm ci installs from the lock file)" % (path, i + 1))
            if re.search(r"\b(curl|wget)\b[^|]*\|\s*(sudo\s+)?(ba|z|da)?sh\b", code):
                risky.append("%s line %d: a download piped into a shell" % (path, i + 1))
            if re.search(r"\bgo\s+install\s+\S+@latest\b", code):
                risky.append("%s line %d: go install @latest" % (path, i + 1))
    if risky:
        r.fail("installs that take whatever is newest: %s" % "; ".join(risky[:15]))
    else:
        r.ok("no unhashed pip install, npm install or piped download in .gitlab-ci.yml, build/*.sh,"
             " build/ci/*.sh or a Dockerfile (%d files)" % len(scripts))

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
    """One item of a rules list as {"if", "when", "narrowed", "unreadable"}."""
    keys = [l for l in item if l.strip() and not l.lstrip().startswith("#")]
    rule = {"if": "", "when": "on_success", "narrowed": False, "unreadable": None}
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
        """The rules a job runs by: its own, else the first parent's that has any
        (extends replaces a list whole), each a dict; None when there are none."""
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
            for p in parents(name):
                found = rules_of(p, depth + 1)
                if found is not None:
                    return found
        return None

    def judged(name):
        """Why a job can be started for unreviewed code, or None."""
        return ("its YAML: %s" % unreadable(name)) if unreadable(name) else admits(rules_of(name))

    # A tag of one of the project's runners, or a variable, which could be one. The
    # runners are registered with more tags than their names (glab api runners/<id>,
    # 26 September 2026): linix with linux, docker and linix, bench150 with windows,
    # bench and bench150. A job asking for any of them can land on one.
    own = re.compile(r"\b(linix|linux|docker|bench150|bench|windows)\b|\$", re.I)
    guard = jobs.get(".own-runners", "")
    guard_ok = judged(".own-runners") is None and PROTECTED in guard
    if guard_ok:
        r.ok(".own-runners creates a job only for a protected ref, and never for a merge request")
    else:
        r.fail(".own-runners no longer demands a protected ref that is not a merge request's")
    on_own, exposed, through_guard, unclear = [], [], 0, []
    for name in real:
        # A job whose extends, key line or merge keys the check cannot follow may take
        # its tags and rules from anywhere: it is refused whatever its tags look like.
        problem = unreadable(name)
        if problem:
            unclear.append("%s (%s)" % (name, problem))
            continue
        tags = []
        for n in [name] + extends(name):
            found = values(jobs.get(n, ""), "tags")
            # Tags the check cannot read may be the project's own runners.
            tags += ["linix"] if found is None else found
        if not any(own.search(t) for t in tags):
            continue
        on_own.append(name)
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
             " %d by .own-runners, %d by rules of their own that ask the same" % (
                 len(on_own), through_guard, len(on_own) - through_guard))

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
]


def self_test():
    """Token-Permissions against SELF_TEST_BASE and each case: the base and the
    cases that must pass are not FAILed, and every other case is."""
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
    print("%d case(s), %d judged wrongly" % (len(SELF_TEST_CASES) + 1, wrong))
    return 1 if wrong else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", default=os.path.join("security", "repo-hygiene"))
    parser.add_argument("--self-test", action="store_true",
                        help="prove Token-Permissions against CI files made to break it, offline, and exit")
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
