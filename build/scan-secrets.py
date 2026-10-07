#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Secrets a change would put into the repository, found before it gets there.

GitLab's push protection, which refuses a push that carries a secret, is GitLab
Ultimate; on the Free tier the secret_detection job (.gitlab/ci/gitlab-security.yml)
finds one only after the push. This is the scan that runs before: in the two git hooks
the repository ships (build/hooks/pre-commit and build/hooks/pre-push, turned on with
`git config core.hooksPath build/hooks`), and in the maintainer's landing, which scans
every commit a batch brings to master and pushes nothing when it finds something
(CONTRIBUTING.md, "Secrets"; SECURITY.md, "Secrets and credentials").

It runs gitleaks - by preference the one in GitLab's secrets analyzer image, pinned by
the digest the secret_detection job names (read from .gitlab/ci/gitlab-security.yml, so
the two never differ), with GitLab's rules (/gitleaks.toml in the image) and the
repository's .gitleaksignore: the same scan the pipeline runs. Where it runs:

  docker    Docker on this machine: the image, with the repository's git directory
            mounted read-only.
  ssh       Docker on another machine that sees this repository's git directory at a
            path of its own, as the project's Linux runner sees the maintainer's
            (HM_SECRET_SCAN_SSH="user@host:/path/to/.git", and HM_SECRET_SCAN_SSH_KEY
            for the key when the ssh configuration does not name it).
  gitleaks  a gitleaks 8.x on PATH (or HM_GITLEAKS), with its own default rules: close
            to GitLab's, not the same, so the pipeline's job may still find what this did
            not.

--engine auto (the default) takes docker when Docker answers, else ssh when
HM_SECRET_SCAN_SSH is set, else gitleaks when there is one; with none of them the scan
cannot run and the commit, push or landing is refused (exit 3) - a scan that did not run
has not said anything is clean. Nothing is downloaded from GitHub.

What it scans:
  --staged           the lines `git commit` is about to add (the index against HEAD);
  --range A..B       every commit git log A..B lists, each with its own diff: what a
                     push of B over A brings;
  --pre-push REMOTE  git's pre-push lines on stdin, each pushed ref's new commits.

Exit codes: 0 nothing found; 1 a possible secret; 2 the scan failed or was asked
wrongly; 3 no scanner. Each finding is printed as gitleaks fingerprints it - with --range,
<commit>:<file>:<rule>:<line>, the form .gitleaksignore takes - and never the secret
itself (--redact).

  python build/scan-secrets.py --staged
  python build/scan-secrets.py --range master..batch41
  python build/scan-secrets.py --self-test
"""

import os
import sys

# Nothing is imported by name from this script's own directory, which Python puts first
# on sys.path: a file put there named as one of Python's modules (json.py, re.py) would
# run as this script's code in a change that leaves the script alone
# (build/check-release-chain.py, PATH_GUARD).
sys.path[:] = [p for p in sys.path if os.path.normcase(os.path.realpath(p or os.curdir)) !=
               os.path.normcase(os.path.dirname(os.path.realpath(__file__)))]

import argparse
import base64
import json
import re
import shutil
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECURITY_YML = ".gitlab/ci/gitlab-security.yml"
IGNORE = ".gitleaksignore"
# The image line of the secret_detection job, pinned by digest. Each line of the
# job is matched one way only (indentation up to its first visible character, or
# blanks), so a job without the line fails in time proportional to its length rather
# than trying every split of every line's indentation.
IMAGE = re.compile(r"^secret_detection:[ \t]*\r?\n(?:(?:[ \t]+\S[^\r\n]*|[ \t]*)\r?\n)*?  image:[ \t]*(\S+@sha256:[0-9a-f]{64})[ \t]*\r?$",
                   re.MULTILINE)
ZERO = re.compile(r"^0+$")
REDACTED_ADVICE = (
    "A real secret: do not push. Take it out of every commit that has it (the commits are not pushed yet, so they can "
    "be rewritten), and revoke it where it was issued - once it has been in a commit, assume it is known "
    "(SECURITY.md, 'Secrets and credentials').\n"
    "Not a secret - a test fixture, an example key from a standard: add its fingerprint line to .gitleaksignore in a "
    "commit that says what it is. With --staged the fingerprint has no commit yet: commit with --no-verify after "
    "reading it, and add the line the range scan then prints.")


class ScanError(Exception):
    pass


def git(repo, *arguments, **kwargs):
    done = subprocess.run(["git", "-C", repo, "-c", "core.quotePath=false"] + list(arguments), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, input=kwargs.get("input"))
    if kwargs.get("check", True) and done.returncode != 0:
        raise ScanError("git %s: %s" % (" ".join(arguments), done.stderr.decode("utf-8", "replace").strip()))
    return done


def text_of(repo, rev, path):
    """A file as the commit rev has it, or the working tree's when rev is None; '' when it has none."""
    if rev:
        done = git(repo, "show", "%s:%s" % (rev, path), check=False)
        return done.stdout.decode("utf-8", "replace") if done.returncode == 0 else ""
    try:
        with open(os.path.join(repo, path), encoding="utf-8") as f:
            return f.read()
    except OSError:
        return ""


def image_of(text):
    match = IMAGE.search(text)
    if not match:
        raise ScanError("%s has no secret_detection job with an image pinned by digest to take the scanner from" % SECURITY_YML)
    return match.group(1)


# ---------------------------------------------------------------- what is scanned

def staged_stream(diff):
    """The lines a staged diff adds, as one text, and where each came from: [(path, line)]."""
    lines, places = [], []
    path, number = None, 0
    for raw in diff.split("\n"):
        if raw.startswith("+++ "):
            target = raw[4:]
            path = None if target == "/dev/null" else (target[2:] if target.startswith("b/") else target)
            continue
        if raw.startswith("--- ") or raw.startswith("diff --git ") or raw.startswith("index "):
            continue
        hunk = re.match(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@", raw)
        if hunk:
            number = int(hunk.group(1))
            continue
        if path and raw.startswith("+"):
            lines.append(raw[1:])
            places.append((path, number))
            number += 1
    return "\n".join(lines) + ("\n" if lines else ""), places


def pre_push_ranges(stdin_text, remote):
    """git log options for each ref a push updates: its new commits. A deleted ref brings none."""
    ranges = []
    for line in stdin_text.splitlines():
        parts = line.split()
        if len(parts) != 4:
            continue
        _, local, _, remote_sha = parts
        if ZERO.match(local):
            continue
        if ZERO.match(remote_sha):
            ranges.append("%s --not --remotes=%s" % (local, remote) if remote else "%s --not --remotes" % local)
        else:
            ranges.append("%s..%s" % (remote_sha, local))
    return ranges


def tip_of(log_opts):
    """The commit a range ends at, whose .gitleaksignore the scan reads."""
    first = log_opts.split()[0]
    return first.split("..", 1)[1] if ".." in first else first


def added_ignore_lines(repo, log_opts):
    """The .gitleaksignore lines the range adds, for the reader to check."""
    first = log_opts.split()[0]
    if ".." not in first:
        return []
    base, tip = first.split("..", 1)
    done = git(repo, "diff", "--no-color", "-U0", base, tip, "--", IGNORE, check=False)
    return [l[1:] for l in done.stdout.decode("utf-8", "replace").split("\n")
            if l.startswith("+") and not l.startswith("+++") and l[1:].strip() and not l[1:].lstrip().startswith("#")]


# ---------------------------------------------------------------- the engines

def docker_answers():
    if not shutil.which("docker"):
        return False
    try:
        return subprocess.run(["docker", "version", "--format", "{{.Server.Version}}"], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=20).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def choose(engine):
    if engine != "auto":
        return engine
    if docker_answers():
        return "docker"
    if os.environ.get("HM_SECRET_SCAN_SSH"):
        return "ssh"
    if os.environ.get("HM_GITLEAKS") or shutil.which("gitleaks"):
        return "gitleaks"
    return None


def gitleaks_arguments(mode, log_opts, ignore_path, report_path, config, repo_path):
    arguments = [mode]
    if config:
        arguments += ["--config", config]
    if mode == "git":
        arguments += ["--log-opts", log_opts]
        if ignore_path:
            arguments += ["--gitleaks-ignore-path", ignore_path]
    arguments += ["--redact", "--no-banner", "--no-color", "--exit-code", "1", "--report-format", "json",
                  "--report-path", report_path]
    if mode == "git":
        arguments.append(repo_path)
    return arguments


def container_script(mode, log_opts, tip, image, work):
    """The shell a Docker host runs: the scan in the pinned image, the report printed after
    a marker. The repository's git directory is /repo, read-only; work is /work."""
    quoted = lambda s: "'" + s.replace("'", "'\"'\"'") + "'"
    inner = gitleaks_arguments(mode, log_opts, "/work/ignore", "/work/report.json", "/gitleaks.toml", "/repo")
    check = ""
    if mode == "git":
        check = "git -C /repo cat-file -e %s || { echo 'the scanned repository has no commit %s' >&2; exit 4; }; " % (
            quoted(tip + "^{commit}"), tip.replace("'", ""))
    run = check + "exec gitleaks " + " ".join(quoted(a) for a in inner) + (" < /work/stream" if mode == "stdin" else "")
    return run


def docker_command(image, git_dir, work, script):
    return ["docker", "run", "--rm", "--network", "none", "-v", "%s:/repo:ro" % git_dir, "-v", "%s:/work" % work,
            "-e", "GIT_CONFIG_COUNT=1", "-e", "GIT_CONFIG_KEY_0=safe.directory", "-e", "GIT_CONFIG_VALUE_0=*",
            "--entrypoint", "sh", image, "-c", script]


def work_directory():
    """The directory the container mounts as /work, and the private one it is made in.

    The image's user writes the report in /work, whoever it is, so /work is open to every
    account (0777); it sits in a directory of mkdtemp's own (0700, this user's), so no other
    account on the machine can reach it to read the lines being scanned, plant a link where
    the ignore file or the stream is written, or put a report of its own in place of
    gitleaks'. Docker mounts /work by its path, which the daemon resolves itself.
    """
    private = tempfile.mkdtemp(prefix="scan-secrets-")
    work = os.path.join(private, "work")
    os.mkdir(work)
    os.chmod(work, 0o777)
    return private, work


def run_docker(image, git_dir, mode, log_opts, tip, ignore_text, stream):
    private, work = work_directory()
    try:
        with open(os.path.join(work, "ignore"), "w", encoding="utf-8", newline="\n") as f:
            f.write(ignore_text)
        with open(os.path.join(work, "stream"), "w", encoding="utf-8", newline="\n") as f:
            f.write(stream or "")
        done = subprocess.run(docker_command(image, git_dir, work, container_script(mode, log_opts, tip, image, "/work")),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        report = None
        path = os.path.join(work, "report.json")
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as f:
                report = f.read()
        return done.returncode, report, done.stderr.decode("utf-8", "replace")
    finally:
        shutil.rmtree(private, ignore_errors=True)


MARKER = "----- scan-secrets report -----"


def remote_script(image, git_dir, mode, log_opts, tip, ignore_text, stream):
    blob = lambda s: base64.b64encode(s.encode("utf-8")).decode("ascii")
    script = container_script(mode, log_opts, tip, image, "/work")
    inner = " ".join("'" + a.replace("'", "'\"'\"'") + "'" for a in docker_command(image, git_dir, "$work", script))
    inner = inner.replace("'$work:/work'", "\"$work:/work\"")
    # As work_directory() does here: /work is open to every account, inside a directory of
    # mktemp's own (0700) that no other account on that machine can reach.
    return "\n".join([
        "set -u",
        "private=$(mktemp -d) || exit 2",
        "trap 'rm -rf \"$private\"' EXIT",
        "work=\"$private/work\"",
        "mkdir \"$work\" || exit 2",
        "base64 -d > \"$work/ignore\" <<'IGNORE_EOF'", blob(ignore_text), "IGNORE_EOF",
        "base64 -d > \"$work/stream\" <<'STREAM_EOF'", blob(stream or ""), "STREAM_EOF",
        "chmod 0777 \"$work\"",
        inner + " >&2",
        "code=$?",
        "echo '%s'" % MARKER,
        "[ -f \"$work/report.json\" ] && cat \"$work/report.json\"",
        "exit $code",
        ""])


def run_ssh(image, target, mode, log_opts, tip, ignore_text, stream):
    host, _, git_dir = target.rpartition(":")
    if not host or not git_dir.startswith("/"):
        raise ScanError("HM_SECRET_SCAN_SSH is %r: it is user@host:/path/to/the/repository/.git, the path at which that "
                        "machine sees this repository's git directory" % target)
    command = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15"]
    key = os.environ.get("HM_SECRET_SCAN_SSH_KEY")
    if key:
        command += ["-i", os.path.expanduser(key)]
    command += [host, "sh -s"]
    done = subprocess.run(command, input=remote_script(image, git_dir, mode, log_opts, tip, ignore_text, stream).encode("utf-8"),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = done.stdout.decode("utf-8", "replace")
    report = out.split(MARKER, 1)[1].strip() if MARKER in out else None
    return done.returncode, report or None, done.stderr.decode("utf-8", "replace")


def run_gitleaks(repo, mode, log_opts, ignore_text, stream):
    binary = os.environ.get("HM_GITLEAKS") or shutil.which("gitleaks")
    work = tempfile.mkdtemp(prefix="scan-secrets-")
    try:
        ignore = os.path.join(work, "ignore")
        with open(ignore, "w", encoding="utf-8", newline="\n") as f:
            f.write(ignore_text)
        report_path = os.path.join(work, "report.json")
        done = subprocess.run([binary] + gitleaks_arguments(mode, log_opts, ignore, report_path, None, repo),
                              input=(stream or "").encode("utf-8") if mode == "stdin" else None,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        report = None
        if os.path.isfile(report_path):
            with open(report_path, encoding="utf-8") as f:
                report = f.read()
        return done.returncode, report, done.stderr.decode("utf-8", "replace")
    finally:
        shutil.rmtree(work, ignore_errors=True)


# ---------------------------------------------------------------- the verdict

# A line gitleaks logs at the error level, in zerolog's console form (--no-color):
# "8:22AM ERR [git] fatal: ...".
GITLEAKS_ERROR = re.compile(r"^\S+ ERR\b", re.MULTILINE)


def findings_of(report_text, code, err=""):
    """gitleaks' JSON report, or ScanError when there is none or it is not a whole scan's.

    gitleaks writes the report whenever it stops, so [] alone does not say the scan ran to
    its end. When the git log it reads fails - a range whose base the repository does not
    have, an object that is missing - gitleaks 8.30.1 logs "ERR [git] fatal: ..." and
    'ERR error="stderr is not empty"', then "no leaks found", writes [] and exits 0 (the
    pinned image, 27 September 2026). So [] is a clean scan only with exit 0 and no error
    logged. Findings are findings whatever the code and the log."""
    if report_text is None or not report_text.strip():
        raise ScanError("gitleaks wrote no report: the scan did not run to its end")
    try:
        value = json.loads(report_text)
    except ValueError as e:
        raise ScanError("gitleaks' report is not JSON: %s" % e)
    if not isinstance(value, list):
        raise ScanError("gitleaks' report is not a list of findings")
    if not value and code != 0:
        raise ScanError("gitleaks reported nothing but exited %s: the scan did not run to its end" % code)
    if not value and GITLEAKS_ERROR.search(err or ""):
        raise ScanError("gitleaks reported nothing but logged an error: the scan did not run to its end")
    return value


def describe(finding, places=None):
    line = finding.get("StartLine") or 0
    place = "%s:%s" % (finding.get("File") or "", line)
    if places is not None and 1 <= int(line) <= len(places):
        path, number = places[int(line) - 1]
        place = "%s:%d" % (path, number)
    # A staged line's fingerprint names no commit yet, and is no line .gitleaksignore takes.
    fingerprint = "" if places is not None else (finding.get("Fingerprint") or "")
    commit = (finding.get("Commit") or "")[:12]
    return "  %s  %s%s  [%s]  %s" % (place, ("commit %s  " % commit) if commit else "", fingerprint and ("fingerprint %s" % fingerprint),
                                    finding.get("RuleID") or "", (finding.get("Description") or "")[:100])


def scan(repo, what, engine, say=print):
    """what: ("staged", None) or ("range", log_opts). Returns the exit code."""
    mode = "stdin" if what[0] == "staged" else "git"
    log_opts = what[1] or ""
    places = None
    stream = ""
    if mode == "stdin":
        has_head = git(repo, "rev-parse", "--verify", "-q", "HEAD", check=False).returncode == 0
        base = "HEAD" if has_head else "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
        diff = git(repo, "diff", "--cached", "--no-color", "--no-ext-diff", "--find-renames", "-U0", base).stdout.decode("utf-8", "replace")
        stream, places = staged_stream(diff)
        if not places:
            say("scan-secrets: nothing staged adds a line")
            return 0
        rev = None
        tip = ""
    else:
        tip = tip_of(log_opts)
        if git(repo, "cat-file", "-e", tip + "^{commit}", check=False).returncode != 0:
            raise ScanError("%s is not a commit here" % tip)
        # Every revision the range names, not the tip alone: a range git log refuses is one
        # gitleaks scans as empty (findings_of).
        listed = git(repo, "rev-list", "--count", *log_opts.split(), check=False)
        if listed.returncode != 0:
            raise ScanError("git cannot list %s here: %s" % (log_opts, listed.stderr.decode("utf-8", "replace").strip()))
        rev = tip
    security = text_of(repo, rev, SECURITY_YML) or text_of(repo, None, SECURITY_YML) or text_of(ROOT, None, SECURITY_YML)
    ignore_text = text_of(repo, rev, IGNORE)
    chosen = choose(engine)
    if chosen is None:
        say("scan-secrets: no scanner - neither Docker, nor HM_SECRET_SCAN_SSH, nor gitleaks on PATH. The scan cannot "
            "run, so nothing is let through (CONTRIBUTING.md, 'Secrets', says how to get one).")
        return 3
    if chosen in ("docker", "ssh"):
        image = image_of(security)
        git_dir = git(repo, "rev-parse", "--path-format=absolute", "--git-common-dir").stdout.decode("utf-8").strip()
        if chosen == "docker":
            code, report, err = run_docker(image, git_dir, mode, log_opts, tip, ignore_text, stream)
        else:
            code, report, err = run_ssh(image, os.environ.get("HM_SECRET_SCAN_SSH", ""), mode, log_opts, tip, ignore_text, stream)
        how = "gitleaks in %s with GitLab's rules (%s)" % (image.split("@")[0], "Docker here" if chosen == "docker" else "Docker on " + os.environ["HM_SECRET_SCAN_SSH"].rpartition(":")[0])
    else:
        code, report, err = run_gitleaks(repo, mode, log_opts, ignore_text, stream)
        how = "gitleaks on PATH with its own rules"
    try:
        findings = findings_of(report, code, err)
    except ScanError as e:
        say("scan-secrets: %s (exit %d). What the scanner said:" % (e, code))
        for line in err.strip().split("\n")[-15:]:
            say("  " + line)
        return 2
    if mode == "stdin":
        subject = "%d staged line(s)" % len(places)
    else:
        counted = git(repo, "rev-list", "--count", *log_opts.split(), check=False).stdout.decode("utf-8").strip() or "?"
        subject = "%s commit(s) of %s" % (counted, log_opts)
        added = added_ignore_lines(repo, log_opts)
        if added:
            say("scan-secrets: the range adds %d line(s) to .gitleaksignore, each a finding no scan reports from now on "
                "- read them:" % len(added))
            for line in added:
                say("    " + line)
    if not findings:
        say("scan-secrets: %s scanned by %s - nothing found" % (subject, how))
        return 0
    say("scan-secrets: %s scanned by %s - %d possible secret(s):" % (subject, how, len(findings)))
    for f in findings:
        say(describe(f, places))
    say(REDACTED_ADVICE)
    return 1


# ---------------------------------------------------------------- the self-test

def self_test():
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    diff = ("diff --git a/src/k.cpp b/src/k.cpp\nindex 1..2 100644\n--- a/src/k.cpp\n+++ b/src/k.cpp\n"
            "@@ -3,0 +4,2 @@ ctx\n+const char *key = \"x\";\n+int y;\n@@ -9 +11 @@\n-old\n+new\n"
            "diff --git a/gone.txt b/gone.txt\n--- a/gone.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-bye\n"
            "diff --git a/dir with space/n.txt b/dir with space/n.txt\n--- /dev/null\n+++ b/dir with space/n.txt\n@@ -0,0 +1 @@\n+hello\n")
    stream, places = staged_stream(diff)
    expect(stream == "const char *key = \"x\";\nint y;\nnew\nhello\n", "the staged stream is the added lines alone: %r" % stream)
    expect(places == [("src/k.cpp", 4), ("src/k.cpp", 5), ("src/k.cpp", 11), ("dir with space/n.txt", 1)],
           "each added line keeps its file and its line: %r" % places)
    expect(staged_stream("") == ("", []), "nothing staged is nothing to scan")

    a, b, z = "a" * 40, "b" * 40, "0" * 40
    pushed = "refs/heads/x %s refs/heads/x %s\nrefs/heads/new %s refs/heads/new %s\n(delete) %s refs/heads/old %s\n" % (b, a, b, z, z, a)
    expect(pre_push_ranges(pushed, "gitlab") == ["%s..%s" % (a, b), "%s --not --remotes=gitlab" % b],
           "a push's ranges: an update's new commits, a new ref's commits no remote-tracking ref has, no deletion: %r"
           % pre_push_ranges(pushed, "gitlab"))
    expect(tip_of("%s..%s" % (a, b)) == b and tip_of("%s --not --remotes=gitlab" % b) == b, "the range's tip")

    yml = ("semgrep-sast:\n  image:\n    name: registry.example/semgrep@sha256:%s\n\nsecret_detection:\n"
           "  image: registry.gitlab.com/security-products/secrets:7@sha256:%s\n  needs: []\n" % ("1" * 64, "2" * 64))
    expect(image_of(yml) == "registry.gitlab.com/security-products/secrets:7@sha256:" + "2" * 64, "the image is secret_detection's")
    expect(image_of(yml.replace("\n", "\r\n")).endswith("2" * 64), "a CRLF checkout's file is read the same")
    try:
        image_of(yml.replace("@sha256:" + "2" * 64, ":7"))
        failures.append("an image not pinned by digest is refused")
    except ScanError:
        pass
    own = text_of(ROOT, None, SECURITY_YML)
    try:
        expect("@sha256:" in image_of(own), "the tree's secret_detection image is pinned")
    except ScanError as e:
        failures.append("the tree's %s names the scanner's image: %s" % (SECURITY_YML, e))

    expect(findings_of("[]", 0) == [], "an empty report is a clean scan")
    for bad in (None, "", "{", "{}"):
        try:
            findings_of(bad, 0)
            failures.append("a report that is %r is not taken for a clean scan" % bad)
        except ScanError:
            pass
    for code in (1, 2, 126):
        try:
            findings_of("[]", code)
            failures.append("an empty report with exit %d is not taken for a clean scan" % code)
        except ScanError:
            pass
    # What gitleaks 8.30.1 in the pinned image logged, wrote and exited with for a range
    # whose base the repository does not have (27 September 2026, on linix).
    partial = ("8:22AM ERR [git] fatal: Invalid revision range 1111111111111111111111111111111111111111..HEAD\n"
               "8:22AM ERR error=\"stderr is not empty\"\n8:22AM INF 0 commits scanned.\n"
               "8:22AM INF scanned ~0 bytes (0) in 124ms\n8:22AM INF no leaks found\n")
    try:
        findings_of("[]", 0, partial)
        failures.append("a scan whose git log failed is not taken for a clean scan")
    except ScanError:
        pass
    clean = "8:22AM INF 1 commits scanned.\n8:22AM INF scanned ~2 bytes (2 bytes) in 152ms\n8:22AM INF no leaks found\n"
    expect(findings_of("[]", 0, clean) == [], "a whole scan's log with nothing found is a clean scan")
    expect(len(findings_of('[{"RuleID": "x"}]', 1, partial)) == 1 and len(findings_of('[{"RuleID": "x"}]', 0)) == 1,
           "a finding is a finding whatever gitleaks exited with and logged")
    if git(ROOT, "rev-parse", "--verify", "-q", "HEAD", check=False).returncode == 0:
        # A scanner that would say the range is clean, as gitleaks does of a range git log
        # refuses: the range must be refused before it is asked.
        held_run = globals()["run_gitleaks"]
        asked = []
        globals()["run_gitleaks"] = lambda *a: asked.append(a) or (0, "[]", "")
        try:
            said = []
            code = scan(ROOT, ("range", "1111111111111111111111111111111111111111..HEAD"), "gitleaks", say=said.append)
            failures.append("a range whose base is not here is refused before any scanner runs: exit %s, %s" % (code, said))
        except ScanError:
            expect(not asked, "the scanner is not asked about a range git cannot list")
        finally:
            globals()["run_gitleaks"] = held_run
    leak = {"RuleID": "gitlab_personal_access_token", "Description": "GitLab personal access token", "StartLine": 2,
            "File": "", "Commit": "", "Fingerprint": "stdin:gitlab_personal_access_token:2", "Secret": "REDACTED"}
    shown = describe(leak, places)
    expect("src/k.cpp:5" in shown and "REDACTED" not in shown and "gitlab_personal_access_token" in shown,
           "a staged finding is shown at its file and line, without the secret: %r" % shown)
    ranged = dict(leak, File="a.txt", StartLine=7, Commit=b, Fingerprint="%s:a.txt:gitlab_personal_access_token:7" % b)
    shown = describe(ranged)
    expect("a.txt:7" in shown and "fingerprint %s:a.txt:gitlab_personal_access_token:7" % b in shown,
           "a range's finding is shown with the fingerprint .gitleaksignore takes: %r" % shown)

    script = container_script("git", "%s..%s" % (a, b), b, "img", "/work")
    expect("--gitleaks-ignore-path' '/work/ignore'" in script and "'--redact'" in script and "'/gitleaks.toml'" in script
           and "cat-file -e '%s^{commit}'" % b in script, "the container runs gitleaks with GitLab's rules, the ignore file, redacted: %s" % script)
    script = container_script("stdin", "", "", "img", "/work")
    expect(script.endswith("< /work/stream") and "--log-opts" not in script, "a staged scan reads the stream: %s" % script)
    remote = remote_script("img", "/g/.git", "git", "%s..%s" % (a, b), b, "x", "")
    expect("'--network' 'none'" in remote, "the scanner's container has no network")
    order = [remote.find(step) for step in ("private=$(mktemp -d) || exit 2", "trap 'rm -rf \"$private\"' EXIT",
                                             "work=\"$private/work\"", "mkdir \"$work\" || exit 2", "chmod 0777 \"$work\"")]
    expect(-1 not in order and order == sorted(order) and "chmod 0777 \"$private\"" not in remote,
           "on the ssh host only /work is open to all, inside mktemp's private directory, which the trap removes: %s" % remote)

    private, work = work_directory()
    try:
        expect(os.path.dirname(work) == private and os.path.isdir(work), "the work directory is inside the private one")
        if os.name == "posix":
            expect(os.stat(private).st_mode & 0o777 == 0o700 and os.stat(work).st_mode & 0o777 == 0o777,
                   "the private directory is 0700 and only the work directory in it 0777: %o %o"
                   % (os.stat(private).st_mode & 0o777, os.stat(work).st_mode & 0o777))
    finally:
        shutil.rmtree(private, ignore_errors=True)
    expect(not os.path.exists(private), "the private directory is removed")

    held = dict(os.environ)
    held_which = shutil.which
    try:
        os.environ.pop("HM_SECRET_SCAN_SSH", None)
        os.environ.pop("HM_GITLEAKS", None)
        shutil.which = lambda name: None
        expect(choose("auto") is None, "no Docker, no ssh host and no gitleaks is no scanner")
        os.environ["HM_SECRET_SCAN_SSH"] = "u@h:/g/.git"
        expect(choose("auto") == "ssh", "the ssh host when there is no Docker here")
        out = []
        os.environ.pop("HM_SECRET_SCAN_SSH", None)
        if git(ROOT, "rev-parse", "--verify", "-q", "HEAD~1", check=False).returncode == 0:
            code = scan(ROOT, ("range", "HEAD~1..HEAD"), "auto", say=out.append)
            expect(code == 3 and any("no scanner" in l for l in out), "with no scanner the scan refuses, exit 3: %s %s" % (code, out))
    finally:
        shutil.which = held_which
        os.environ.clear()
        os.environ.update(held)

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    what = parser.add_mutually_exclusive_group()
    what.add_argument("--staged", action="store_true", help="the lines the next commit adds")
    what.add_argument("--range", help="the commits of a git log range, such as master..batch41")
    what.add_argument("--pre-push", metavar="REMOTE", help="git's pre-push lines on stdin")
    what.add_argument("--self-test", action="store_true")
    parser.add_argument("--repo", default=None, help="the repository (default: the one the current directory is in)")
    parser.add_argument("--engine", default="auto", choices=["auto", "docker", "ssh", "gitleaks"])
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    repo = args.repo
    if not repo:
        done = subprocess.run(["git", "rev-parse", "--show-toplevel"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        repo = done.stdout.decode("utf-8").strip() if done.returncode == 0 else ROOT
    try:
        if args.staged:
            return scan(repo, ("staged", None), args.engine)
        if args.range:
            if args.range.startswith("-"):
                raise ScanError("--range is a git log range such as master..batch41, not an option")
            return scan(repo, ("range", args.range), args.engine)
        if args.pre_push is not None:
            worst = 0
            for log_opts in pre_push_ranges(sys.stdin.read(), args.pre_push):
                first = log_opts.split()[0]
                if ".." in first and git(repo, "cat-file", "-e", first.split("..", 1)[0] + "^{commit}", check=False).returncode != 0:
                    # The remote's commit is not here (a force push over someone else's):
                    # every commit no remote-tracking ref has, instead.
                    log_opts = "%s --not --remotes=%s" % (first.split("..", 1)[1], args.pre_push)
                code = scan(repo, ("range", log_opts), args.engine)
                worst = max(worst, code)
            return worst
    except ScanError as e:
        print("scan-secrets: %s" % e)
        return 2
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
