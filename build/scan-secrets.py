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

It runs gitleaks - by preference the one in GitLab's secrets analyzer image, as the
project's registry holds it (build/ci/mirror-secrets.sh) and the secret_detection job
names it, by digest - with GitLab's rules (/gitleaks.toml in the image) and the
repository's .gitleaksignore: the same scan the pipeline runs. The rules are GitLab's
but for the two entries of their global allowlist that left out every path under a
directory named build, bin, obj, target or packages, this tree's build/ among them:
build/ci/secret-detection-rules.sh derives them in the image from GitLab's own file, as
the job does, and refuses when GitLab's allowlist is not the one it was written for.

What it trusts (OWN) is the tree it runs from, read from its files, never the range it
scans: this script, the rules script, and the image the tree's
.gitlab/ci/gitlab-security.yml names for secret_detection - an image and a script
written for each other, so both from one tree. From the range it takes the commits it
scans and its tip's .gitleaksignore, as the pipeline would read it (the working tree's
for --staged), and prints every line the range adds to that file, every line it
changes in the rules script, and the image its tip names when the range moves it. A
line that carries gitleaks' inline allow marker (the word gitleaks, then ":allow") is
a finding here like any other (--ignore-gitleaks-allow), so that only a .gitleaksignore
line, which is shown, waives one; secret_detection's analyzer, which cannot be given
the flag, would take the marker as a waiver, and build/check-gitleaksignore.py fails a
tree that carries it. So the range chooses neither the scanner nor its rules, and what
it waives is shown. That holds when this script runs from a tree whose copies of OWN
are the range's base's, as the landing's are: build/land-via-mr.py refuses to run from a
checkout whose copies are not master's. Run from a tree whose copies are the range's
own (a batch's worktree), the batch's copies judge the batch; from one whose copies are
neither (a checkout behind the base, or edited), that tree's do; and the scan says
which. Where it runs:

  docker    Docker on this machine: the image, with the repository's git directory
            mounted read-only.
  ssh       Docker on another machine that sees this repository's git directory at a
            path of its own, as the project's Linux runner sees the maintainer's
            (HM_SECRET_SCAN_SSH="user@host:/path/to/.git", and HM_SECRET_SCAN_SSH_KEY
            for the key when the ssh configuration does not name it).
  gitleaks  a gitleaks 8.x on PATH (or HM_GITLEAKS), with its own default rules: close
            to GitLab's, not the same, so the pipeline's job may still find what this did
            not. One too old to take --ignore-gitleaks-allow refuses it, and the scan
            fails (exit 2).

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
import functools
import json
import re
import shutil
import stat
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECURITY_YML = ".gitlab/ci/gitlab-security.yml"
IGNORE = ".gitleaksignore"
# The script that makes the rules gitleaks runs with from the image's /gitleaks.toml,
# GitLab's, taking out the entries of its allowlist that leave out build/ and the like;
# and the step that runs it in the container, before gitleaks. A refusal is exit 5,
# after which there is no report, and the scan fails (findings_of).
RULES_SCRIPT = "build/ci/secret-detection-rules.sh"
RULES_STEP = "sh /work/rules.sh /gitleaks.toml /work/gitleaks.toml || exit 5; "
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


def image_or_none(text):
    try:
        return image_of(text)
    except ScanError:
        return None


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


def changed_rules_lines(repo, log_opts):
    """The lines the range adds to or takes from the rules script, which decides what the
    scans read from the next pipeline on, for the reader to check."""
    first = log_opts.split()[0]
    if ".." not in first:
        return []
    base, tip = first.split("..", 1)
    done = git(repo, "diff", "--no-color", "-U0", base, tip, "--", RULES_SCRIPT, check=False)
    return [l for l in done.stdout.decode("utf-8", "replace").split("\n")
            if (l.startswith("+") or l.startswith("-")) and not l.startswith("+++") and not l.startswith("---") and l[1:].strip()]


def rules_script():
    """The rules script as this script's own tree has it - never the scanned range's -
    since the container runs it. It reads no list of paths from a tree: the rules are
    derived from GitLab's file in the image. ScanError when there is none."""
    text = text_of(ROOT, None, RULES_SCRIPT)
    if not text.strip():
        raise ScanError("there is no %s, which makes the scan's rules" % RULES_SCRIPT)
    return text.replace("\r\n", "\n")


# What this scan takes from the tree it runs from, and nothing from the range: this
# script, the rules script, and the CI file whose secret_detection image it runs.
OWN = ("build/scan-secrets.py", RULES_SCRIPT, SECURITY_YML)


def base_of(log_opts):
    """The commit a range starts from (A of A..B), or None when it names none."""
    first = log_opts.split()[0]
    return first.split("..", 1)[0] if ".." in first else None


def copies_of_own(repo, log_opts):
    """[(path, the range's base's copy, its tip's, this tree's)] for each file of OWN, CRLF
    read as LF, the CI file by its secret_detection image alone (None for none); [] for a
    range with no base."""
    base = base_of(log_opts)
    if base is None:
        return []
    tip = tip_of(log_opts)
    found = []
    for path in OWN:
        base_copy, tip_copy, own_copy = [text.replace("\r\n", "\n") for text in
                                         (text_of(repo, base, path), text_of(repo, tip, path), text_of(ROOT, None, path))]
        if path == SECURITY_YML:
            base_copy, tip_copy, own_copy = image_or_none(base_copy), image_or_none(tip_copy), image_or_none(own_copy)
        found.append((path, base_copy, tip_copy, own_copy))
    return found


def judged_by_its_own(repo, log_opts):
    """The files of OWN that the range changes and that this tree has as the range's tip
    has them: a scan run from the range's own tree, such as a batch's worktree, whose
    copies judge the range."""
    return [path for path, base_copy, tip_copy, own_copy in copies_of_own(repo, log_opts) if own_copy == tip_copy != base_copy]


def judged_by_neither(repo, log_opts):
    """The files of OWN that this tree has as neither the range's base nor its tip has
    them: a scan run from a checkout behind the base, or edited, whose copies judge the
    range though they are neither what it lands on nor its own."""
    return [path for path, base_copy, tip_copy, own_copy in copies_of_own(repo, log_opts)
            if own_copy != base_copy and own_copy != tip_copy]


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
    # gitleaks leaves out, by default, a finding whose line carries its inline allow
    # marker: a waiver written on the secret's own line, which no one is shown and no
    # owner accepts. Here it is a finding, which only a .gitleaksignore line waives.
    arguments += ["--ignore-gitleaks-allow"]
    arguments += ["--redact", "--no-banner", "--no-color", "--exit-code", "1", "--report-format", "json",
                  "--report-path", report_path]
    if mode == "git":
        arguments.append(repo_path)
    return arguments


def container_script(mode, log_opts, tip, image, work):
    """The shell a Docker host runs: the rules made from the image's own, then the scan in
    the pinned image. The repository's git directory is /repo, read-only; work is /work,
    which holds the ignore file, the stream and the rules script."""
    quoted = lambda s: "'" + s.replace("'", "'\"'\"'") + "'"
    inner = gitleaks_arguments(mode, log_opts, "/work/ignore", "/work/report.json", "/work/gitleaks.toml", "/repo")
    check = ""
    if mode == "git":
        check = "git -C /repo cat-file -e %s || { echo 'the scanned repository has no commit %s' >&2; exit 4; }; " % (
            quoted(tip + "^{commit}"), tip.replace("'", ""))
    run = check + RULES_STEP + "exec gitleaks " + " ".join(quoted(a) for a in inner) + (" < /work/stream" if mode == "stdin" else "")
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


def run_docker(image, git_dir, mode, log_opts, tip, ignore_text, stream, rules):
    private, work = work_directory()
    try:
        for name, text in (("ignore", ignore_text), ("stream", stream or ""), ("rules.sh", rules)):
            with open(os.path.join(work, name), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
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


def remote_script(image, git_dir, mode, log_opts, tip, ignore_text, stream, rules):
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
        "base64 -d > \"$work/rules.sh\" <<'RULES_EOF'", blob(rules), "RULES_EOF",
        "chmod 0777 \"$work\"",
        inner + " >&2",
        "code=$?",
        "echo '%s'" % MARKER,
        "[ -f \"$work/report.json\" ] && cat \"$work/report.json\"",
        "exit $code",
        ""])


def run_ssh(image, target, mode, log_opts, tip, ignore_text, stream, rules):
    host, _, git_dir = target.rpartition(":")
    if not host or not git_dir.startswith("/"):
        raise ScanError("HM_SECRET_SCAN_SSH is %r: it is user@host:/path/to/the/repository/.git, the path at which that "
                        "machine sees this repository's git directory" % target)
    command = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15"]
    key = os.environ.get("HM_SECRET_SCAN_SSH_KEY")
    if key:
        command += ["-i", os.path.expanduser(key)]
    command += [host, "sh -s"]
    # This scan's rules are bound to remote_script here, so that the call below keeps the
    # line build/ci/semgrep-baseline.txt holds as triaged on 26 September 2026 (a finding's
    # fingerprint is its line's text): what the triage read is still what the call does -
    # an argv list, no shell here, and every file the host is given, the rules script now
    # among them, base64 in a heredoc whose delimiter is quoted.
    remote_script = functools.partial(globals()["remote_script"], rules=rules)
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
    ignore_text = text_of(repo, rev, IGNORE)
    chosen = choose(engine)
    if chosen is None:
        say("scan-secrets: no scanner - neither Docker, nor HM_SECRET_SCAN_SSH, nor gitleaks on PATH. The scan cannot "
            "run, so nothing is let through (CONTRIBUTING.md, 'Secrets', says how to get one).")
        return 3
    image = None
    if chosen in ("docker", "ssh"):
        # The scanner's image and the rules script both this tree's own, never the range's:
        # the two are written for each other (the script refuses an image whose allowlist
        # is not the one it names), and the range chooses neither what scans it nor how.
        image = image_of(text_of(ROOT, None, SECURITY_YML))
        rules = rules_script()
        git_dir = git(repo, "rev-parse", "--path-format=absolute", "--git-common-dir").stdout.decode("utf-8").strip()
        if chosen == "docker":
            code, report, err = run_docker(image, git_dir, mode, log_opts, tip, ignore_text, stream, rules)
        else:
            code, report, err = run_ssh(image, os.environ.get("HM_SECRET_SCAN_SSH", ""), mode, log_opts, tip, ignore_text, stream, rules)
        how = "gitleaks in %s with GitLab's rules, build/ and the like read (%s)" % (image.split("@")[0], "Docker here" if chosen == "docker" else "Docker on " + os.environ["HM_SECRET_SCAN_SSH"].rpartition(":")[0])
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
        changed = changed_rules_lines(repo, log_opts)
        if changed:
            say("scan-secrets: the range changes %d line(s) of %s, which decides what both scans read from the next "
                "pipeline on (this scan ran this tree's copy) - read them:" % (len(changed), RULES_SCRIPT))
            for line in changed:
                say("    " + line)
        named = lambda paths: paths[0] if len(paths) == 1 else ", ".join(paths[:-1]) + " and " + paths[-1]
        if image:
            # The image the range's tip names, when the range moves it - or when the range
            # has no base to tell: one its base names too is not the range's move, and a
            # tree that runs another than both is said below.
            base = base_of(log_opts)
            theirs = image_or_none(text_of(repo, tip, SECURITY_YML))
            if theirs != image and (base is None or theirs != image_or_none(text_of(repo, base, SECURITY_YML))):
                say("scan-secrets: the range's tip names %s for secret_detection, where this tree names %s, which this "
                    "scan ran: this scan did not run the range's, which for a batch master's secret_detection runs "
                    "first, once the batch has landed - compare the two over the history before "
                    "(ContinuousIntegration.md, \"Re-pinning secret_detection's analyzer\")"
                    % (theirs or "no image pinned by digest", image))
        own = judged_by_its_own(repo, log_opts)
        if own:
            say("scan-secrets: this scan ran from a tree whose %s %s the range's own, which the range changes: the range "
                "was judged by its own copies. A landing refuses such a tree (build/land-via-mr.py), and runs from a "
                "checkout of master (ContinuousIntegration.md, \"What the secret scans read\")"
                % (named(own), "is" if len(own) == 1 else "are"))
        neither = judged_by_neither(repo, log_opts)
        if neither:
            say("scan-secrets: this scan ran from a tree whose %s %s neither the range's base's nor its tip's (the CI "
                "file by its secret_detection image) - a checkout behind the base, or edited: the range was judged by "
                "that tree's copies, not by those of what it lands on. A landing refuses such a checkout "
                "(build/land-via-mr.py): bring it to the base, unedited (ContinuousIntegration.md, \"What the secret "
                "scans read\")" % (named(neither), "is" if len(neither) == 1 else "are"))
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

    mirrored = "registry.gitlab.com/hmailserver/hmailserver/mirror/secrets:7.40.0-222222222222@sha256:"
    yml = ("semgrep-sast:\n  image:\n    name: registry.example/semgrep@sha256:%s\n\nsecret_detection:\n"
           "  # a comment before the image\n  image: %s%s\n  needs: []\n" % ("1" * 64, mirrored, "2" * 64))
    expect(image_of(yml) == mirrored + "2" * 64, "the image is secret_detection's")
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
    expect("--gitleaks-ignore-path' '/work/ignore'" in script and "'--redact'" in script and "'--config' '/work/gitleaks.toml'" in script
           and "'/gitleaks.toml'" not in script and "cat-file -e '%s^{commit}'" % b in script,
           "the container runs gitleaks with the rules made there, the ignore file, redacted: %s" % script)
    expect(0 <= script.find("sh /work/rules.sh /gitleaks.toml /work/gitleaks.toml || exit 5; ") < script.find("exec gitleaks"),
           "the rules are made from the image's own before gitleaks runs, a refusal ending the scan: %s" % script)
    expect("'--ignore-gitleaks-allow'" in script, "a range scan in the container reports a line marked as allowed: %s" % script)
    script = container_script("stdin", "", "", "img", "/work")
    expect(script.endswith("< /work/stream") and "--log-opts" not in script and RULES_STEP in script,
           "a staged scan reads the stream, with the same rules: %s" % script)
    expect("'--ignore-gitleaks-allow'" in script, "a staged scan in the container reports a line marked as allowed: %s" % script)
    # gitleaks on PATH is given the same arguments: a line that carries gitleaks' inline
    # allow marker is a finding, which only a .gitleaksignore line waives.
    for mode in ("git", "stdin"):
        arguments = gitleaks_arguments(mode, "%s..%s" % (a, b), "/i", "/r.json", None, "/repo")
        expect("--ignore-gitleaks-allow" in arguments, "gitleaks on PATH reports a line marked as allowed (%s): %s" % (mode, arguments))
    remote = remote_script("img", "/g/.git", "git", "%s..%s" % (a, b), b, "x", "", "#!/bin/sh\necho rules\n")
    expect("base64 -d > \"$work/rules.sh\"" in remote and base64.b64encode(b"#!/bin/sh\necho rules\n").decode("ascii") in remote
           and "allowlist" not in remote, "the ssh host is given the rules script, and nothing else to make the rules of: %s" % remote)
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

    rules_self_test(expect)

    # The range scans, over a repository of the self-test's own (throwaway_range), so
    # that each runs whatever the depth of this tree's checkout.
    work = tempfile.mkdtemp(prefix="scan-secrets-range-")
    try:
        try:
            commits = throwaway_range(work)
        except ScanError as e:
            commits = None
            failures.append("a repository of %d commits is made for the range scans: %s" % (len(RANGE_COMMITS), e))
        if commits:
            range_self_test(work, commits, expect)
    finally:
        # git writes its objects read-only, which Windows does not delete as they are.
        for base, _, names in os.walk(work):
            for name in names:
                try:
                    os.chmod(os.path.join(base, name), stat.S_IREAD | stat.S_IWRITE)
                except OSError:
                    pass
        shutil.rmtree(work, ignore_errors=True)
    expect(not os.path.exists(work), "the range scans' repository is removed")

    # And over ssh: run_ssh, which binds the rules to remote_script where it calls it on
    # the line the semgrep baseline holds, gives the host the script.
    held_subprocess_run = subprocess.run
    sent = []
    subprocess.run = lambda command, **kw: sent.append(kw.get("input", b"").decode("utf-8")) or \
        subprocess.CompletedProcess(command, 1, b"", b"")
    try:
        run_ssh("img", "u@h:/g/.git", "git", "%s..%s" % (a, b), b, "x", "", "#!/bin/sh\necho rules\n")
        expect(sent and base64.b64encode(b"#!/bin/sh\necho rules\n").decode("ascii") in sent[0],
               "run_ssh gives the host the rules script: %s" % sent)
    finally:
        subprocess.run = held_subprocess_run

    held = dict(os.environ)
    held_which = shutil.which
    try:
        os.environ.pop("HM_SECRET_SCAN_SSH", None)
        os.environ.pop("HM_GITLEAKS", None)
        shutil.which = lambda name: None
        expect(choose("auto") is None, "no Docker, no ssh host and no gitleaks is no scanner")
        os.environ["HM_SECRET_SCAN_SSH"] = "u@h:/g/.git"
        expect(choose("auto") == "ssh", "the ssh host when there is no Docker here")
    finally:
        shutil.which = held_which
        os.environ.clear()
        os.environ.update(held)

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


# throwaway_range's commits, each made on the one before it but the last, "stale", which
# is made on "tip":
#   base      a secret_detection image, a rules script and a .gitleaksignore of its own;
#   tip       base..tip moves the image, changes the rules script and adds a line to
#             .gitleaksignore (RANGE_IMAGES, RANGE_RULES, RANGE_IGNORED);
#   own       tip..own brings this tree's copies of OWN - the CI file a file of its own
#             that names this tree's image - as a batch that changes them has them when
#             it is scanned from its own worktree;
#   same      own..same changes build/x.txt alone, both its ends this tree's: a landing's
#             usual range, scanned from a checkout of master, over a batch that leaves
#             OWN alone;
#   unpinned  same..unpinned names no image pinned by digest for secret_detection;
#   stale     tip..stale changes build/x.txt alone, both its ends naming an image and
#             carrying a rules script that are not this tree's: a scan from a checkout
#             behind the range's base.
RANGE_COMMITS = ("base", "tip", "own", "same", "unpinned", "stale")
RANGE_IMAGES = ("registry.example/base/secrets:1.0.0-%s@sha256:%s" % ("3" * 12, "3" * 64),
                "registry.example/tip/secrets:2.0.0-%s@sha256:%s" % ("4" * 12, "4" * 64))
RANGE_RULES = ("#!/bin/sh\necho the base's rules\n", "#!/bin/sh\necho the range's own rules\n")
RANGE_IGNORED = "%s:build/x.txt:gitlab_personal_access_token:1" % ("5" * 40)


def throwaway_range(work):
    """A repository of the commits RANGE_COMMITS names, made in work for the self-test:
    {name: commit}. The range scans read it, not this tree's history, which the checkers
    job fetches one commit deep (GIT_DEPTH 1): there this tree has no HEAD~1, and a test
    of its own range would be left out without a word. Made with git's plumbing, so that
    no hook runs and nothing is signed. ScanError when git refuses, or this tree's CI file
    names no image."""
    git(work, "init", "-q")
    job = "secret_detection:\n  image: %s\n  needs: []\n"
    own = dict((path, text_of(ROOT, None, path).replace("\r\n", "\n")) for path in OWN)
    own[SECURITY_YML] = job % image_of(own[SECURITY_YML])
    files = {"base": {SECURITY_YML: job % RANGE_IMAGES[0], RULES_SCRIPT: RANGE_RULES[0], IGNORE: "# the base's\n",
                      "build/x.txt": "nothing to find\n"},
             "tip": {SECURITY_YML: job % RANGE_IMAGES[1], RULES_SCRIPT: RANGE_RULES[1], IGNORE: "# the base's\n%s\n" % RANGE_IGNORED},
             "stale": {"build/x.txt": "nothing to find, behind the base\n"},
             "own": own,
             "same": {"build/x.txt": "nothing to find, from this tree\n"},
             "unpinned": {SECURITY_YML: "secret_detection:\n  needs: []\n"}}
    parents = {"base": None, "tip": "base", "stale": "tip", "own": "tip", "same": "own", "unpinned": "same"}
    made = {}
    for name in RANGE_COMMITS:
        parent = made.get(parents[name])
        git(work, "read-tree", *([parent] if parent else ["--empty"]))
        for path, text in sorted(files[name].items()):
            blob = git(work, "hash-object", "-w", "--stdin", input=text.encode("utf-8")).stdout.decode("ascii").strip()
            git(work, "update-index", "--add", "--cacheinfo", "100644,%s,%s" % (blob, path))
        tree = git(work, "write-tree").stdout.decode("ascii").strip()
        commit = git(work, "-c", "user.name=scan-secrets self-test", "-c", "user.email=self-test@example.invalid",
                     "-c", "commit.gpgsign=false", "commit-tree", "-m", "self-test: " + name,
                     *(["-p", parent] if parent else []), tree)
        made[name] = commit.stdout.decode("ascii").strip()
    git(work, "update-ref", "HEAD", made[RANGE_COMMITS[-1]])
    return made


def range_self_test(repo, commits, expect):
    """The range scans, over throwaway_range's repository, with the scanners stood in for."""
    held = dict((name, globals()[name]) for name in ("run_gitleaks", "run_docker", "run_ssh"))
    held_environ = dict(os.environ)
    held_which = shutil.which
    asked = []
    between = lambda base, tip: "%s..%s" % (commits[base], commits[tip])
    # What the scan says of the image the range's tip names, of a range judged by its own
    # copies, and of one judged by copies that are neither its base's nor its tip's.
    moved = "for secret_detection, where this tree names"
    judged = "judged by its own"
    neither = "whose %s, %s and %s are neither the range's base's nor its tip's" % OWN
    try:
        for name in held:
            globals()[name] = (lambda engine: lambda *a: asked.append((engine,) + a) or (0, "[]", ""))(name[len("run_"):])
        # A scanner that would say the range is clean, as gitleaks does of a range git log
        # refuses: the range must be refused before it is asked.
        try:
            said = []
            code = scan(repo, ("range", "1111111111111111111111111111111111111111..HEAD"), "gitleaks", say=said.append)
            expect(False, "a range whose base is not here is refused before any scanner runs: exit %s, %s" % (code, said))
        except ScanError:
            expect(not asked, "the scanner is not asked about a range git cannot list")
        # The container is handed this script's own rules script and runs in this tree's own
        # image, whatever the range's tip names, with the tip's .gitleaksignore; and the
        # reader is shown the line the range adds to the one, the lines it changes in the
        # other, the image its tip names that this scan did not run, and that this tree's
        # copies, which judged the range, are neither the range's base's nor its own.
        own = rules_script()
        own_image = image_of(text_of(ROOT, None, SECURITY_YML))
        os.environ["HM_SECRET_SCAN_SSH"] = "u@h:/g/.git"
        for engine in ("docker", "ssh"):
            del asked[:]
            said = []
            try:
                code = scan(repo, ("range", between("base", "tip")), engine, say=said.append)
            except ScanError as e:
                expect(False, "a range scan with %s stood in for: %s" % (engine, e))
                continue
            handed = asked[0] if len(asked) == 1 and asked[0][0] == engine else None
            expect(code == 0 and handed is not None and own.strip() and handed[-1] == own and "the range's own" not in handed[-1],
                   "with %s the container is given this script's own %s, not the range's: %s %s" % (engine, RULES_SCRIPT, code, said))
            expect(handed is not None and handed[1] == own_image and own_image not in RANGE_IMAGES,
                   "with %s the scan runs in this tree's secret_detection image, %s, not the range's: %s"
                   % (engine, own_image, handed and handed[1]))
            expect(handed is not None and RANGE_IGNORED in handed[-3],
                   "with %s the container is given the range's tip's %s: %s" % (engine, IGNORE, handed and handed[-3]))
            shown = "\n".join(said)
            expect("adds 1 line(s) to %s" % IGNORE in shown and RANGE_IGNORED in shown
                   and "changes 2 line(s) of %s" % RULES_SCRIPT in shown and "+echo the range's own rules" in shown,
                   "with %s the reader is shown the line the range adds to %s and the lines it changes in %s: %s"
                   % (engine, IGNORE, RULES_SCRIPT, shown))
            expect("the range's tip names %s %s %s, which this scan ran" % (RANGE_IMAGES[1], moved, own_image) in shown
                   and judged not in shown,
                   "with %s the reader is shown the image the range's tip names, which this scan did not run: %s"
                   % (engine, shown))
            expect(neither in shown, "with %s the reader is shown that this tree's copies are neither the range's base's "
                   "nor its tip's: %s" % (engine, shown))
        for (base, tip), (wanted, what) in (
                # Both ends name an image and carry a rules script that are not this
                # tree's: the image is not the range's move, and the scan says that this
                # tree's copies judged the range.
                (("tip", "stale"), ((neither,), "a range that leaves OWN alone, scanned from a tree whose copies are "
                                                "not its base's")),
                # Run from a tree whose copies of OWN are the range's own - here the range
                # brings this tree's - the scan says that the range was judged by them.
                (("tip", "own"), (("whose %s, %s and %s are the range's own" % OWN, judged),
                                  "a range scanned from a tree whose copies are the range's own")),
                # A landing's usual range: both ends and this tree alike, nothing to say.
                (("own", "same"), ((), "a range that leaves OWN alone, scanned from a tree whose copies are its base's")),
                # The tip names no image pinned by digest: shown, as any other.
                (("same", "unpinned"), (("the range's tip names no image pinned by digest %s %s" % (moved, own_image),),
                                        "a range whose tip names no image pinned by digest"))):
            del asked[:]
            said = []
            try:
                code = scan(repo, ("range", between(base, tip)), "docker", say=said.append)
            except ScanError as e:
                expect(False, "%s: %s" % (what, e))
                continue
            shown = "\n".join(said)
            unwanted = [note for note in (moved, judged, neither) if note in shown and not any(note in w for w in wanted)]
            expect(code == 0 and len(asked) == 1 and all(w in shown for w in wanted) and not unwanted,
                   "%s: %s, and nothing of %s: exit %s, %s" % (what, wanted, unwanted, code, shown))
        # With no scanner the scan refuses, exit 3.
        del asked[:]
        os.environ.pop("HM_SECRET_SCAN_SSH", None)
        os.environ.pop("HM_GITLEAKS", None)
        shutil.which = lambda name: None
        said = []
        code = scan(repo, ("range", between("base", "tip")), "auto", say=said.append)
        expect(code == 3 and not asked and any("no scanner" in l for l in said),
               "with no scanner the scan refuses, exit 3: %s %s" % (code, said))
    finally:
        globals().update(held)
        shutil.which = held_which
        os.environ.clear()
        os.environ.update(held_environ)


def rules_self_test(expect):
    """build/ci/secret-detection-rules.sh over fixtures, where there is a sh to run it: the
    script as the tree has it but for the hash of the GitLab table it was written for,
    which is the fixture's; what it writes read as gitleaks reads it."""
    import hashlib
    sh = shutil.which("sh")
    path = os.path.join(ROOT, RULES_SCRIPT)
    source = text_of(ROOT, None, RULES_SCRIPT).replace("\r\n", "\n")
    pinned = re.findall(r"^GITLAB_TABLE=([0-9a-f]{64})$", source, re.MULTILINE)
    expect(len(pinned) == 1, "%s names one GitLab table, by its hash: %s" % (RULES_SCRIPT, pinned))
    if not sh:
        print("note: no sh on PATH, so %s was not run over its fixtures here (the checkers job runs it)" % RULES_SCRIPT)
        return
    if not os.path.isfile(path) or not pinned:
        expect(False, "%s is in the tree, and names the GitLab table it changes" % RULES_SCRIPT)
        return
    rules_part = ("# rule-set version: 0.0.1\n[[rules]]\nid = 'x'\nregex = '''x_[a-z]{8}'''\n\n"
                  "[[rules]]\nid = 'y'\nregex = '''y_[a-z]{8}'''\n[[rules.allowlists]]\npaths = ['''z''']\n\n")
    left_out = "    '''(?:^|/)(?:target|build|bin|obj)(?:/.*)?$''',\n"
    dependencies = "    '''(?:^|/)(?:node_modules|bower_components|packages)(?:/.*)?$''',\n"
    kept = "    '''(?:^|/)(?:node_modules|bower_components)(?:/.*)?$''',\n"
    gitlab_table = ("[[allowlists]]\ndescription = \"global allow lists\"\npaths = [\n    '''gitleaks\\.toml''',\n" +
                    dependencies + left_out + "    '''(?i)\\.(?:png|svg)$''',\n    '''(?:^|/)\\.(?:git|vs)(?:/.*)?$'''\n]\n")
    hash_of = lambda table: hashlib.sha256(table.encode("utf-8")).hexdigest()
    work = tempfile.mkdtemp(prefix="scan-secrets-rules-")
    try:
        def run(gitlab, written_for):
            script = re.sub(r"^GITLAB_TABLE=[0-9a-f]{64}$", "GITLAB_TABLE=" + written_for, source, flags=re.MULTILINE)
            for name, text in (("rules.sh", script), ("gitlab.toml", gitlab)):
                with open(os.path.join(work, name), "w", encoding="utf-8", newline="") as f:
                    f.write(text)
            out = os.path.join(work, "out.toml")
            if os.path.exists(out):
                os.remove(out)
            done = subprocess.run([sh, os.path.join(work, "rules.sh"), os.path.join(work, "gitlab.toml"), out],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            written = None
            if os.path.isfile(out):
                with open(out, encoding="utf-8", newline="") as f:
                    written = f.read()
            return done.returncode, written, done.stderr.decode("utf-8", "replace")

        derived = rules_part + gitlab_table.replace(left_out, "").replace(dependencies, kept)
        code, written, err = run(rules_part + gitlab_table, hash_of(gitlab_table))
        expect(code == 0 and written == derived, "GitLab's rules, but for the build, bin, obj, target and packages entries of "
               "their table, and nothing else changed: %s %r %s" % (code, written, err))
        try:
            import tomllib
        except ImportError:
            tomllib = None
            print("note: Python %s has no tomllib, so the rules made were not read as TOML here" % sys.version.split()[0])
        if tomllib is not None and code == 0:
            try:
                tables = tomllib.loads(written).get("allowlists", [])
            except ValueError as e:
                tables = []
                expect(False, "the rules made are TOML: %s" % e)
            paths = [re.compile(p) for t in tables for p in t.get("paths", [])]
            leaves_out = lambda name: any(p.search(name) for p in paths)
            expect(len(tables) == 1 and paths, "the rules made hold one [[allowlists]] table, of paths: %s" % tables)
            for name in ("build/ci/linux-build.sh", "build/hooks/pre-push", "hmailserver/build/x.cpp", "bin/x.cfg", "a/obj/x.json",
                         "target/x.properties", "packages/x.config", "hmailserver/test/packages/x.json", "snap/local/bin/x"):
                expect(not leaves_out(name), "%s is read by both scans: no path of the rules made leaves it out" % name)
            for name in ("node_modules/x/index.js", "web/bower_components/x.js", "docs/a.png", ".git/config"):
                expect(leaves_out(name), "%s stays out, as GitLab's rules leave it out" % name)
        # An entry of a rule's own allowlist, before the global table, is GitLab's still.
        own = rules_part + "[[rules]]\nid = 'w'\nregex = '''w_[a-z]{8}'''\n[[rules.allowlists]]\npaths = [\n" + left_out + "]\n\n"
        code, written, err = run(own + gitlab_table, hash_of(gitlab_table))
        expect(code == 0 and written is not None and written.startswith(own), "a rule's own allowlist is left as it is: %s %s"
               % (code, err))
        # Refused, nothing written: a GitLab table it was not written for, both hashes named;
        # two tables; and a table in which a change does not apply to exactly one line.
        moved = gitlab_table.replace("packages)", "packages|vendor)")
        code, written, err = run(rules_part + moved, hash_of(gitlab_table))
        expect(code == 1 and written is None and hash_of(moved) in err and hash_of(gitlab_table) in err,
               "a GitLab table this was not written for is refused, both hashes named, nothing written: %s %s" % (code, err))
        code, written, err = run(rules_part + gitlab_table + gitlab_table, hash_of(gitlab_table))
        expect(code == 1 and written is None and "2 [[allowlists]]" in err, "two GitLab tables are refused: %s %s" % (code, err))
        for what, table in (("without the build entry", gitlab_table.replace(left_out, "")),
                            ("without the packages entry", gitlab_table.replace(dependencies, "")),
                            ("with the build entry twice", gitlab_table.replace(left_out, left_out + left_out)),
                            ("with packages written otherwise", gitlab_table.replace("|packages)", "|packages )"))):
            code, written, err = run(rules_part + table, hash_of(table))
            expect(code == 1 and written is None and "not exactly one line" in err,
                   "a GitLab table %s, its hash the one the script names, is refused: %s %s" % (what, code, err))
    finally:
        shutil.rmtree(work, ignore_errors=True)


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
