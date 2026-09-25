#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The repository-hygiene checks OpenSSF Scorecard ran, answered from GitLab.

Scorecard (.github/workflows/scorecard.yml) stopped with the GitHub account on 18
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
                       key in .github/allowed_signers, which git verifies here.
  Security-Policy      SECURITY.md, with a way to report a vulnerability privately.
  Pinned-Dependencies  every image in .gitlab-ci.yml and every FROM in a Dockerfile
                       pinned by digest (or, for the CI image, by the tag made from
                       its Dockerfile's hash that build/check-ci-image.py enforces);
                       every action a kept GitHub workflow uses pinned by commit;
                       no pip install without hashes, npm install, or piped curl in
                       a CI script; a NuGet lock file beside every project that
                       has package references.
  Binary-Artifacts     no executable or compiled file tracked in git (by content,
                       not only by extension).
  Token-Permissions    no job that a merge request can start reaches the
                       project's own runners; no CI script handles CI_JOB_TOKEN or
                       another credential; no id_tokens or secrets on such a job;
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
    """{tag: (verified, text)} for each tag, by git verify-tag against .github/allowed_signers; None if git cannot."""
    signers = os.path.join(ROOT, ".github", "allowed_signers")
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
                r.fail("tag %s: not verified against .github/allowed_signers - %s" % (tag, text))
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
    r = Result("Pinned-Dependencies", "images, base images, actions and installs pinned to a digest, hash or lock file")
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

    workflows = [f for f in files if f.startswith(".github/workflows/") and f.endswith((".yml", ".yaml"))]
    actions = loose_actions = 0
    loose_list = []
    for path in workflows:
        for i, line in enumerate(read(path).splitlines()):
            m = re.match(r"^\s*(?:-\s*)?uses:\s*([^\s#]+)", line)
            if not m or m.group(1).startswith("./") or m.group(1).startswith("docker://"):
                continue
            actions += 1
            ref = m.group(1).rsplit("@", 1)[-1] if "@" in m.group(1) else ""
            # The SLSA generator's reusable workflow has to be called by its tag: its
            # provenance names the tag, and the verifier refuses a commit. Scorecard
            # exempts it for the same reason.
            if m.group(1).startswith("slsa-framework/slsa-github-generator/") and re.fullmatch(r"v\d+\.\d+\.\d+", ref):
                continue
            if not re.fullmatch(r"[0-9a-f]{40}", ref):
                loose_actions += 1
                loose_list.append("%s line %d: %s" % (path, i + 1, m.group(1)))
    if loose_actions:
        r.fail("%d GitHub action reference(s) not pinned by commit: %s" % (loose_actions, "; ".join(loose_list[:10])))
    elif workflows:
        r.ok("all %d action references in the %d kept GitHub workflows pinned by commit (they do not run on GitLab)" % (
            actions, len(workflows)))

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

def ci_jobs(text):
    """{top-level key: its block's text} for the CI file, without a YAML library."""
    blocks, name, body = {}, None, []
    for line in text.splitlines():
        m = re.match(r"^([A-Za-z0-9_.\-]+):\s*(#.*)?$", line)
        if m:
            if name:
                blocks[name] = "\n".join(body)
            name, body = m.group(1), []
        elif name is not None:
            body.append(line)
    if name:
        blocks[name] = "\n".join(body)
    return blocks


def check_token_permissions(files):
    r = Result("Token-Permissions", "no credential in reach of code nobody has reviewed")
    path = ".gitlab-ci.yml"
    if not os.path.exists(os.path.join(ROOT, path)):
        r.fail("no .gitlab-ci.yml")
        return r
    text = read(path)
    jobs = ci_jobs(text)
    reserved = {"stages", "workflow", "default", "variables", "include"}

    def extends(name, depth=0):
        body = jobs.get(name, "")
        m = re.search(r"^\s+extends:\s*\[?([^\]\n]+)\]?", body, re.M)
        parents = [p.strip().strip("'\"") for p in m.group(1).split(",")] if m else []
        chain = list(parents)
        if depth < 5:
            for p in parents:
                chain += extends(p, depth + 1)
        return chain

    own = re.compile(r"\b(linix|bench150|bench)\b")
    guard = jobs.get(".own-runners", "")
    guard_ok = ('$CI_COMMIT_REF_PROTECTED == "true"' in guard
                and '$CI_PIPELINE_SOURCE != "merge_request_event"' in guard)
    if guard_ok:
        r.ok(".own-runners creates a job only for a protected ref, and never for a merge request")
    else:
        r.fail(".own-runners no longer demands a protected ref that is not a merge request's")
    exposed = []
    for name, body in jobs.items():
        if name in reserved or name.startswith("."):
            continue
        chain = [name] + extends(name)
        tags = " ".join(re.findall(r"^\s+tags:\s*(.*)$", "\n".join(jobs.get(n, "") for n in chain), re.M))
        if not own.search(tags):
            continue
        if ".own-runners" not in chain:
            exposed.append(name)
    if exposed:
        r.fail("job(s) on the project's own runners without the .own-runners rules: %s" % ", ".join(exposed))
    else:
        r.ok("every job on linix or bench150 extends .own-runners")

    scripts = [path] + [f for f in files if re.match(r"^build/ci/[^/]+\.(sh|ps1)$", f)]
    handled = []
    for s in scripts:
        for i, line in enumerate(read(s).splitlines()):
            if re.match(r"^\s*#", line):
                continue
            for m in re.finditer(r"\$\{?(CI_JOB_TOKEN|CI_REGISTRY_PASSWORD|CI_DEPLOY_PASSWORD|CI_JOB_JWT\w*|"
                                 r"[A-Z0-9_]*(?:TOKEN|SECRET)[A-Z0-9_]*)\}?", line):
                handled.append("%s line %d: %s" % (s, i + 1, m.group(1)))
    if handled:
        r.fail("CI scripts handle a credential: %s" % "; ".join(handled[:10]))
    else:
        r.ok("no CI script reads CI_JOB_TOKEN or any *TOKEN* or *SECRET* variable (%d files)" % len(scripts))

    for name, body in jobs.items():
        if re.search(r"^\s+(id_tokens|secrets):", body, re.M):
            chain = [name] + extends(name)
            if ".own-runners" not in chain:
                r.fail("job %s asks for id_tokens or secrets and can run for a merge request" % name)
            else:
                r.note("job %s asks for id_tokens or secrets, on a protected ref only" % name)
    if re.search(r"CI_DEBUG_TRACE\s*:\s*['\"]?true", text, re.I):
        r.fail("CI_DEBUG_TRACE is set: every variable, masked or not, would be printed to a public job log")
    else:
        r.ok("CI_DEBUG_TRACE is set nowhere (the job logs are public)")
    literal = re.findall(r"^\s+([A-Z0-9_]*(?:PASSWORD|TOKEN|SECRET)[A-Z0-9_]*):\s*(\S+)", text, re.M)
    for key, value in literal:
        if key == "POSTGRES_PASSWORD" and value.strip("'\"") == "hmtest":
            r.note("POSTGRES_PASSWORD is written in the file: the throwaway password of the linux-suite job's"
                   " own PostgreSQL service container, which nothing outside that job can reach")
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


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", default=os.path.join("security", "repo-hygiene"))
    args = parser.parse_args()
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
