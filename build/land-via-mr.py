#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Land a batch* branch on master through a GitLab merge request.

A maintainer's change reaches master this way (CONTRIBUTING.md, "Merge requests";
GOVERNANCE.md, "How decisions are made"). In order, and stopping at the first
refusal:

  1. The branch is a batch* branch, it is on top of the remote's master (the
     project merges by fast-forward only), it adds no merge commit, and every
     commit it adds carries its author's Signed-off-by (build/sign-off.py, the
     same check the sign-off job runs; commits already in one of the project's
     v* release tags are left out), and the secret scan of those commits
     (build/scan-secrets.py: gitleaks with GitLab's rules but for the entries
     of their allowlist that leave out build/ and the like, as
     build/ci/secret-detection-rules.sh derives them, and the tip's
     .gitleaksignore, in Docker here or where HM_SECRET_SCAN_SSH points) finds
     nothing and could run. The scan, its rules script and the analyzer image
     it runs, the one this checkout's secret_detection names, are this
     checkout's own, not the batch's, and so are this script and the sign-off
     check (CHECKOUT_COPIES); it refuses (exit 2) unless each is as the
     remote's master has it, working tree and all - not a checkout behind
     master, nor an edited one, nor the batch's own worktree when the batch
     changes one. So the batch has no say in what scans it or how, but for the
     .gitleaksignore lines it adds, which the scan prints; a line that carries
     gitleaks' inline allow marker is a finding of the scan like any other.
     The one way past that refusal is --own-copies REASON, for a batch that
     repairs what master's copies cannot land - an analyzer image no longer
     pulled, the scan or this script broken: run from the batch's own worktree,
     every one of those files as the batch's tip has it (a checkout behind
     master, an edited one, or one that has master's copy of a file the batch
     changes beside the batch's of another is refused all the same), the batch
     is judged by its own copies, which the scan says, and the landing record
     keeps REASON and the files.
     A batch branch's pipeline does not run the
     secret_detection job and its merge request runs none, so this scan is the
     one that reads a batch before it is on GitLab. And a batch that changes the
     gates' own files (GATE_FILES: the gate scripts; the CI files that say whether
     and how their jobs run - .gitlab-ci.yml, whose workflow, defaults and includes
     every job takes, and the files under .gitlab/ci/ the jobs are in or take a part
     from; this script and check-release-chain.py; sign-off.py and scan-secrets.py,
     which this runs, and secret-detection-rules.sh, which makes both secret scans'
     rules; and the source scans' scripts, static-analysis.sh and semgrep.sh) needs
     --gates-read REASON, saying a person read that change whole
     (exit 9 without it). Nothing is pushed until all of this holds.
  2. The branch is pushed. batch* is a protected branch, so its pipeline runs
     the whole gate on the project's own runners (docs/ContinuousIntegration.md);
     a merge request from it runs no pipeline of its own, and the branch's
     pipeline is the one GitLab holds the merge request to.
  3. A merge request into master is opened, or the open one for the branch is
     found: running this again resumes where it stopped.
  4. The pipeline of exactly the branch's tip is waited for, once the merge
     request's head is that tip: GitLab moves the head to a pushed commit a
     moment after the push, so a head that is not yet the tip is read again
     every HEAD_POLL_SECONDS for up to HEAD_WAIT_SECONDS from the push (or from
     finding the branch at the tip already), saying what it waits for, before
     it refuses; a head that leaves the tip once it was there refuses at once.
     Anything but success refuses: failed, canceled, skipped, or blocked on a
     manual job.
     Then its jobs are read, every page of them, since GitLab calls a pipeline a
     success with a job that failed while allowed to fail and with a job a rule
     never created. The change gates semgrep-sast, semgrep-sast-gate and
     mr-dependency-check, and checkers, which holds the CI files to them, must
     each have succeeded (GATE_JOBS), and so must the
     source scans static-analysis and semgrep (SOURCE_SCAN_JOBS) and the Windows
     suite's two halves, windows-gate and windows-gate-s1vm (WINDOWS_GATE_JOBS),
     each of which a bench busy past its lock's wait ends with exit 10, allowed
     to fail, unless every file the batch changes from master is Markdown at the
     root or under a directory whose name begins with a dot - the changes for
     which .own-runners creates none of them. A gate that is missing, skipped, or
     failed refuses (exit 5), naming it - a half a busy bench let pass is gated
     by retrying its job; the statuses go into the landing record, and so do those of
     hosted-runners and compute-quota (HOSTED_CHECK_JOBS), which no landing waits
     for, or that neither was in the pipeline. The pipeline must be
     the project's own (its project_id), not one a fork ran on runners of its
     owner's choosing; and each gate that finds the change's base itself
     (BASE_GATES) must say in its log that it judged the batch against master's
     tip, where the batch leaves it, so that a base a CI file named shows. When the
     batch changes the gates' code (GATE_CODE), each must also say that master's
     copy of its script judged the batch and passed it (BASE_COPY_SCRIPTS), so that
     a change of the code that decides whether that copy runs shows too.
  5. GitLab must call the merge request mergeable: the pipeline passed, every
     thread is resolved, it is not a draft, and master has not moved.
  6. Approvals are read. On GitLab's Free tier anyone with the Developer role
     can press Approve, but no rule can require it. What this records is whether
     somebody other than the author approved: not the merge request's author,
     not an author of one of its commits, and not another account of either
     (ONE_PERSON), and given after the tip reached the merge request (its diff
     versions), not on an earlier push. Without one it REFUSES (exit 7): from the
     first landing after 6.3.4 every merge request needs Zain Ul Abidin's approval
     (the owner's, for Zain's own), the owner's decision of 26 September 2026
     (GOVERNANCE.md, "Review"). The owner alone may override that for one landing,
     with --land-without-approval REASON: it is refused unless the account glab is
     logged in as is the owner's (OWNER), the reason is recorded in the landing
     note and the log, and the change is to be reviewed afterwards.
  7. It merges by fast-forward, at exactly the tip it checked (the merge API's
     sha), never squashed, and checks that the remote's master is that tip.
  8. It records the landing: a note on the merge request (the pipeline, its
     gate jobs, the sign-off, the approvals, who landed it) and a line in
     land-via-mr.log in the repository's git directory. Then it moves the local
     master to the tip when no worktree has master checked out - when one has,
     it names it, to be brought to the tip before the next landing, which
     refuses it if the batch changed one of CHECKOUT_COPIES - and deletes the
     branch, remote and local, where it can.

It needs glab, logged in to gitlab.com as a Maintainer (only Maintainers may
push a batch* branch or merge into master), and git. What it writes to GitLab - the
merge request, its landing record, the merge - goes to glab as a JSON body on its
standard input, never on its command line (Glab, below).

Usage:
  python3 build/land-via-mr.py BRANCH [--remote NAME] [--dry-run] [--no-wait]
                               [--force-push] [--timeout-hours H] [--poll-seconds S]
                               [--land-without-approval REASON] [--gates-read REASON]
                               [--own-copies REASON]
  python3 build/land-via-mr.py --self-test

  --dry-run      the checks of step 1 and what would happen; no push, no API write
  --no-wait      push and open the merge request, then stop; run again to finish
  --force-push   the remote branch holds commits the local one does not (it was
                 rebased): replace it, with a lease on what the remote had
  --land-without-approval REASON
                 the owner's override of the approval rule for this one landing
                 (a security fix inside its disclosure window, a broken master):
                 lands without a second person's approval, and records REASON
  --gates-read REASON
                 the batch changes the gates' own files (GATE_FILES), and a person
                 has read that change whole: REASON says who and what, and is
                 recorded
  --own-copies REASON
                 master's copies of the files a landing runs (CHECKOUT_COPIES)
                 cannot land anything, and the batch repairs them: this runs from
                 the batch's own worktree, every one of them as its tip has it,
                 and the batch is judged by them; REASON says who and why, and is
                 recorded with the files

Exit codes: 0 landed (or the dry run or --no-wait passed), 2 usage, a git or
GitLab error, or a checkout whose files the landing runs are not master's
(CHECKOUT_COPIES), or on --own-copies not the batch's tip's, 3 the branch
cannot land by fast-forward, 4 a commit is not signed off, 5 the merge
request, its pipeline or a gate job in it did not pass, 6
GitLab would not merge, 7 no second person approved and the owner gave no
override, or the override was refused, 8 the secret scan found a possible
secret in the batch's commits or could not run, 9 the batch changes the gates' own
files and --gates-read was not given.
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
import datetime
import importlib.util
import inspect
import io
import json
import re
import shutil
import subprocess
import tempfile
import time
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = "86730559"
HOST = "gitlab.com"
# The project's paths: the group's, where it has been since 29 September 2026, and the
# personal one it began at, which GitLab redirects to the group's (a clone made before
# the move still names it); a remote whose URL names one of them is the project.
PROJECT_PATHS = ("hmailserver/hmailserver", "Progressiverobot/hmailserver")
TARGET = "master"
BRANCH = re.compile(r"^batch[A-Za-z0-9._-]*$")
# Accounts that are one person (GOVERNANCE.md, "Maintainers"): an approval from one of
# them is not a second person's on a merge request another of them wrote.
ONE_PERSON = [{"progressiverobot", "chrisholloway5"}]
# The owner's accounts: only a landing run as one of them may use --land-without-approval.
OWNER = {"progressiverobot", "chrisholloway5"}
# Commit author emails whose GitLab account is known, for the same question.
EMAIL_ACCOUNT = {
    "chris.holloway@progressiverobot.com": "progressiverobot",
    "chris@progressiverobot.com": "progressiverobot",
}
PENDING = {"created", "waiting_for_resource", "preparing", "pending", "running", "scheduled"}
SETTLING = {"checking", "unchecked", "approvals_syncing", "preparing", "ci_still_running"}
# GitLab moves a merge request's head to a commit pushed to its branch a moment after the
# push, not with it: on 14 October 2026 the landing of batch47 read its merge request
# straight after pushing the new tip and found the head still the commit before
# (1df1dc3e794e, not 2bd79d38170e), and refused; run again, it went on. Until the head has
# been the tip once, step 4 reads it again every HEAD_POLL_SECONDS, for up to
# HEAD_WAIT_SECONDS from the push - or from finding the branch at the tip already - and
# refuses only then; a head that leaves the tip once it was there refuses at once.
HEAD_WAIT_SECONDS = 300
HEAD_POLL_SECONDS = 10
# The jobs a landing needs to have succeeded, beyond the pipeline as a whole: a pipeline
# succeeds with a job that failed while it was allowed to fail, and with a job a rule
# left out of it, and either is a gate that did not hold the batch. The change gates
# (SECURITY.md, "Vulnerability management policy") and checkers, whose
# check-release-chain.py holds the CI files to what the gates need - a CI file that set
# checkers allow_failure would take that away unseen - always; the two source scans
# unless every file the batch changes is a document (documents_only).
# build/check-release-chain.py holds these names to the jobs the CI files define.
GATE_JOBS = ("semgrep-sast", "semgrep-sast-gate", "mr-dependency-check", "checkers")
SOURCE_SCAN_JOBS = ("static-analysis", "semgrep")
# The Windows suite's two halves, on bench150 and S1-WINBENCH (.gitlab-ci.yml), on the
# same terms as the source scans: a bench whose lock another holder keeps past
# HM_GATE_LOCK_WAIT_MINUTES ends its half with exit 10, which a batch's pipeline lets
# fail - the pipeline green, with a warning, and that half of the suite never run - so
# each must have succeeded itself; retrying the half's job gates it
# (docs/ContinuousIntegration.md, "The Windows gate").
WINDOWS_GATE_JOBS = ("windows-gate", "windows-gate-s1vm")
# The checks of a pipeline for which a switch sends a class of jobs to GitLab's hosted
# runners (.gitlab/ci/hosted-runners.yml), which no landing waits for: a failure of
# hosted-runners fails the pipeline itself, and compute-quota is allowed to fail whatever
# it says of the compute minutes. The landing record gives what each came to, so that what
# compute-quota said is seen at each landing.
HOSTED_CHECK_JOBS = ("hosted-runners", "compute-quota")
# The gates that find the change's base themselves and say it in their log ("The base:
# <commit>, ..."): a landing holds it to where the batch leaves master, so that a base a
# CI file named would show.
BASE_GATES = ("semgrep-sast-gate", "mr-dependency-check")
BASE_LINE = re.compile(r"The base: ([0-9a-f]{40}(?:[0-9a-f]{24})?), ")
# The gates' code, as security-gate.py's GATE_CODE names it: a batch that changes one of
# these files is judged by the base's copy of each gate's script too (base_copy_verdict),
# which says in the gate's log how it ended - "The base's copy of <script> exited 0." -
# or that the base has no copy of the script. A landing requires one of the two in each
# gate's log, so that a change of the code that decides whether the copy runs shows.
# build/check-release-chain.py holds GATE_CODE to security-gate.py's, and the scripts to
# the gates' jobs.
GATE_CODE = ("build/ci/security-gate.py", "build/ci/security-findings.py", "build/ci/mr-dependency-check.py",
             "build/ci/dependency-scan.py", "build/ci/osv-offline-db.py")
BASE_COPY_SCRIPTS = {"semgrep-sast-gate": "build/ci/security-gate.py", "mr-dependency-check": "build/ci/mr-dependency-check.py"}
BASE_COPY_RAN = "The base's copy of %s exited 0."
BASE_COPY_NONE = "The base has no %s: the change's copy alone judges it."
# The gates' own files: what judges a change, what says whether and how its jobs run, and
# what holds the jobs to it - the gates' code; .gitlab-ci.yml, whose workflow rules,
# defaults and includes every job takes; the files the gate jobs are in or take a part
# from (mr-checks.yml, gitlab-security.yml, no-github.yml's .no-github); this script and
# check-release-chain.py; the two scripts this runs before anything is pushed, and the
# script that makes the secret scans' rules, which decides what both of them read; and the
# source scans' scripts, static-analysis.sh and semgrep.sh, whose lines outside their
# marks (which security-gate.py holds as a narrowing) still say how the job ends and
# what it hands security-findings.py. A batch that changes one is judged by the base's
# copy of the gate scripts too, but only a person reads such a change whole, and
# --gates-read says that one has; it goes into the landing record.
# build/check-release-chain.py holds this list to GATE_CODE, to .gitlab-ci.yml, to the
# files the gate jobs' extends and !reference reach and to security-gate.py's
# SCAN_SCRIPTS.
GATE_FILES = GATE_CODE + (".gitlab-ci.yml", ".gitlab/ci/mr-checks.yml", ".gitlab/ci/gitlab-security.yml",
                          ".gitlab/ci/no-github.yml", "build/land-via-mr.py", "build/check-release-chain.py",
                          "build/sign-off.py", "build/scan-secrets.py", "build/ci/secret-detection-rules.sh",
                          "build/ci/static-analysis.sh", "build/ci/semgrep.sh")
# The files a landing runs from its own checkout - the tree this script is in - and not
# from the batch: this script, the sign-off check, the secret scan, the rules script the
# scan hands its container, and the CI file whose secret_detection image it runs in
# (build/scan-secrets.py's OWN). They judge the batch, so each must be as the base has
# it, working tree and all: a checkout behind master, or edited, would judge the batch
# by other code than master's without a word, and the batch's own worktree by the
# batch's (checkout_differs) - unless --own-copies says who lands a batch that repairs
# them by its own, and why, every one as its tip has it. build/check-release-chain.py
# holds this list to the scripts the landing runs and the CI file.
CHECKOUT_COPIES = ("build/land-via-mr.py", "build/sign-off.py", "build/scan-secrets.py",
                   "build/ci/secret-detection-rules.sh", ".gitlab/ci/gitlab-security.yml")
# A pipeline's jobs are read a page at a time; a batch's has more than a hundred.
JOBS_PER_PAGE = 100
MOST_JOB_PAGES = 50


class Refused(Exception):
    def __init__(self, code, message):
        Exception.__init__(self, message)
        self.code = code


class ApiError(Exception):
    pass


def _load_sign_off():
    spec = importlib.util.spec_from_file_location("sign_off", os.path.join(ROOT, "build", "sign-off.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def checkout_differs(repo, base, root=ROOT):
    """The files of CHECKOUT_COPIES whose copy in root's working tree is not base's, as
    repo, which has base, holds it: a file one of them lacks counts as another, and CRLF
    as LF, as a checkout on Windows writes it."""
    differ = []
    for path in CHECKOUT_COPIES:
        done = subprocess.run(["git", "-C", repo, "cat-file", "blob", "%s:%s" % (base, path)], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        theirs = done.stdout.replace(b"\r\n", b"\n") if done.returncode == 0 else None
        try:
            with open(os.path.join(root, *path.split("/")), "rb") as f:
                ours = f.read().replace(b"\r\n", b"\n")
        except OSError:
            ours = None
        if ours != theirs:
            differ.append(path)
    return differ


def secret_scan(repo, base, tip):
    """build/scan-secrets.py over the commits base..tip (CONTRIBUTING.md, "Secrets"):
    its exit code - 0 nothing found, 1 a possible secret, 2 the scan failed, 3 no
    scanner - and what it printed."""
    done = subprocess.run([sys.executable, os.path.join(ROOT, "build", "scan-secrets.py"), "--repo", repo,
                           "--range", "%s..%s" % (base, tip)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return done.returncode, done.stdout.decode("utf-8", "replace")


class Git(object):
    def __init__(self, repo):
        self.repo = repo

    def run(self, *args, **kw):
        done = subprocess.run(["git", "-C", self.repo] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = done.stdout.decode("utf-8", "replace")
        if kw.get("check", True) and done.returncode != 0:
            raise Refused(2, "git %s failed: %s" % (" ".join(args), done.stderr.decode("utf-8", "replace").strip()))
        return out if kw.get("check", True) else (done.returncode, out)

    def ok(self, *args):
        return self.run(*args, check=False)[0] == 0

    def sha(self, rev):
        code, out = self.run("rev-parse", "--verify", "--quiet", rev + "^{commit}", check=False)
        return out.strip() if code == 0 else None


class Glab(object):
    """GitLab's REST API through glab, which holds the maintainer's login.

    A request's fields go to glab as one JSON object on its standard input
    (glab api --input -, with Content-Type: application/json), never on its command
    line: each was an -f or -F argument until 13 October 2026, and batch47's merge
    request description - 200 commit lines, 50,565 characters - made a command line
    longer than the 32,767 characters Windows takes, so that the landing died after
    it had pushed (WinError 206) and the merge request was opened by hand. A boolean
    stays a JSON boolean and an integer a JSON number, as -F made them; every other
    value goes as its text, as -f sent it. A GET's fields stay -f and -F arguments,
    which glab makes query parameters of: no GET here has any. program is the command
    glab is started as; the self-test gives it a stand-in that reads what it is sent."""

    def __init__(self, host, program=None):
        self.host = host
        self.program = list(program or ["glab"])

    def request(self, method, path, fields=None):
        """The command line and the body (bytes, or None) a request is sent with."""
        argv = self.program + ["api", "--hostname", self.host, "-X", method, path]
        if not fields:
            return argv, None
        if method == "GET":
            for key, value in fields.items():
                if isinstance(value, bool):
                    argv += ["-F", "%s=%s" % (key, "true" if value else "false")]
                elif isinstance(value, int):
                    argv += ["-F", "%s=%d" % (key, value)]
                else:
                    argv += ["-f", "%s=%s" % (key, value)]
            return argv, None
        body = {}
        for key, value in fields.items():
            body[key] = value if isinstance(value, (bool, int)) else "%s" % (value,)
        return (argv + ["-H", "Content-Type: application/json", "--input", "-"],
                json.dumps(body, ensure_ascii=False).encode("utf-8"))

    def __call__(self, method, path, fields=None):
        argv, body = self.request(method, path, fields)
        if body is None:
            done = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        else:
            done = subprocess.run(argv, input=body, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = done.stdout.decode("utf-8", "replace")
        if done.returncode != 0:
            raise ApiError("%s %s: %s %s" % (method, path, done.stderr.decode("utf-8", "replace").strip(), out.strip()[:300]))
        return json.loads(out) if out.strip() else None

    def text(self, path):
        """What a GET of path answers, as text: a job's log, which is not JSON."""
        done = subprocess.run(self.program + ["api", "--hostname", self.host, "-X", "GET", path], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        if done.returncode != 0:
            raise ApiError("GET %s: %s" % (path, done.stderr.decode("utf-8", "replace").strip()[:300]))
        return done.stdout.decode("utf-8", "replace")


def q(text):
    return urllib.parse.quote(text, safe="")


def detect_remote(git):
    for line in git.run("remote", "-v").splitlines():
        parts = line.split()
        if len(parts) >= 2 and any(re.search(r"[:/]%s(\.git)?/?$" % re.escape(p), parts[1], re.IGNORECASE) for p in PROJECT_PATHS):
            if parts[0] == "origin":
                return "origin"
    for line in git.run("remote", "-v").splitlines():
        parts = line.split()
        if len(parts) >= 2 and any(re.search(r"[:/]%s(\.git)?/?$" % re.escape(p), parts[1], re.IGNORECASE) for p in PROJECT_PATHS):
            return parts[0]
    raise Refused(2, "no remote of this repository is the GitLab project (%s); name one with --remote" % " or ".join(PROJECT_PATHS))


def release_tags(git, remote):
    """The commits of the remote's v* tags that are in this repository, peeled."""
    code, out = git.run("ls-remote", "--tags", remote, check=False)
    if code != 0:
        raise Refused(2, "git ls-remote --tags %s failed" % remote)
    peeled = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 2 or not parts[1].startswith("refs/tags/v"):
            continue
        name = parts[1][len("refs/tags/"):]
        if name.endswith("^{}"):
            peeled[name[:-3]] = parts[0]
        else:
            peeled.setdefault(name, parts[0])
    return sorted(sha for sha in set(peeled.values()) if git.sha(sha))


def person_groups(username):
    name = (username or "").lower()
    for group in ONE_PERSON:
        if name in group:
            return set(group)
    return {name} if name else set()


def second_people(approvers, mr_author, commit_authors):
    """The approvers who are not the author: not the merge request's author, not an author
    of one of its commits, and not another account of either. commit_authors is
    [(name, email)]; approvers [{"username", "name"}]."""
    excluded = person_groups(mr_author)
    names = set()
    for name, email in commit_authors:
        names.add((name or "").strip().lower())
        account = EMAIL_ACCOUNT.get((email or "").strip().lower())
        if account:
            excluded |= person_groups(account)
    found = []
    for a in approvers:
        username = (a.get("username") or "").lower()
        if username in excluded or (a.get("name") or "").strip().lower() in names:
            continue
        found.append(a)
    return found


def when(text):
    """A GitLab timestamp (2026-09-26T10:00:00.000Z) as an aware datetime, or None."""
    if not text:
        return None
    try:
        return datetime.datetime.strptime(re.sub(r"\.\d+", "", text.replace("Z", "+00:00")).replace("+00:00", "+0000"),
                                          "%Y-%m-%dT%H:%M:%S%z")
    except ValueError:
        return None


def tip_arrived(api, project, iid, tip):
    """When the merge request got TIP as its head: the earliest of its diff versions for
    that commit (the versions API, on every tier), or None where it cannot be read."""
    try:
        versions = api("GET", "projects/%s/merge_requests/%d/versions" % (project, iid)) or []
    except ApiError:
        return None
    times = [when(v.get("created_at")) for v in versions if v.get("head_commit_sha") == tip]
    times = [t for t in times if t]
    return min(times) if times else None


def current_approvals(approvers, arrived):
    """(the approvals given on the tip, those given before it reached the merge request).
    Whether a push takes the earlier approvals off is a setting of the paid tiers, which
    the Free tier's API does not show, so an approval counts for the tip only when it was
    given after the tip arrived; one whose time is unknown does not, once that is known.
    Where the tip's arrival cannot be read, every approval is taken as it stands."""
    if arrived is None:
        return list(approvers), []
    now, before = [], []
    for a in approvers:
        at = when(a.get("at"))
        (now if at and at >= arrived else before).append(a)
    return now, before


def approvals(api, project, iid):
    """(the approvers [{"username", "name", "at"}], where they were read). The approvals API
    first; its approved_by is what the Approve button fills. Where the tier refuses it,
    the merge request's system notes, which record every approval and its withdrawal."""
    try:
        answer = api("GET", "projects/%s/merge_requests/%d/approvals" % (project, iid)) or {}
        if "approved_by" in answer:
            return [{"username": (e.get("user") or {}).get("username", ""), "name": (e.get("user") or {}).get("name", ""),
                     "at": e.get("approved_at") or ""}
                    for e in answer.get("approved_by") or []], "the approvals API"
    except ApiError:
        pass
    current = {}
    page = 1
    while True:
        notes = api("GET", "projects/%s/merge_requests/%d/notes?sort=asc&order_by=created_at&per_page=100&page=%d" % (project, iid, page)) or []
        for note in notes:
            if not note.get("system"):
                continue
            body = (note.get("body") or "").strip().lower()
            who = note.get("author") or {}
            key = (who.get("username") or "").lower()
            if body.startswith("approved this merge request"):
                current[key] = {"username": who.get("username", ""), "name": who.get("name", ""), "at": note.get("created_at") or ""}
            elif body.startswith("unapproved this merge request"):
                current.pop(key, None)
        if len(notes) < 100:
            break
        page += 1
    return list(current.values()), "the merge request's system notes"


def documents_only(paths):
    """Whether every path is Markdown at the root of the tree or under a directory whose
    name begins with a dot: the changes for which the not-docs-only list of .own-runners
    (.gitlab-ci.yml) creates none of the project's own runners' jobs, the two source scans
    among them. No path at all is not a change of documents."""
    return bool(paths) and all(p.endswith(".md") and ("/" not in p or p.startswith(".")) for p in paths)


def pipeline_jobs(api, project, pipeline_id):
    """{name: job} for every job of the pipeline, the latest attempt of each name, read a
    page of JOBS_PER_PAGE at a time until a page is short."""
    jobs = {}
    for page in range(1, MOST_JOB_PAGES + 1):
        listed = api("GET", "projects/%s/pipelines/%s/jobs?per_page=%d&page=%d" % (
            project, pipeline_id, JOBS_PER_PAGE, page)) or []
        for job in listed:
            name = job.get("name") or ""
            if name not in jobs or (job.get("id") or 0) > (jobs[name].get("id") or 0):
                jobs[name] = job
        if len(listed) < JOBS_PER_PAGE:
            return jobs
    raise Refused(2, "pipeline #%s has more than %d pages of jobs; its jobs were not all read" % (pipeline_id, MOST_JOB_PAGES))


def required_jobs(jobs, changed):
    """[(name, status, allowed to fail)] for each job the landing needs: GATE_JOBS always,
    SOURCE_SCAN_JOBS and WINDOWS_GATE_JOBS unless the batch changes documents only. A job
    not in the pipeline has the status "not in the pipeline"."""
    names = list(GATE_JOBS) + ([] if documents_only(changed) else list(SOURCE_SCAN_JOBS) + list(WINDOWS_GATE_JOBS))
    return [(name, (jobs.get(name) or {}).get("status") or "not in the pipeline", bool((jobs.get(name) or {}).get("allow_failure")))
            for name in names]


def gate_logs(api, project, jobs):
    """{gate: [the lines of its log that are its own]} for each of BASE_GATES, read from
    the log of its latest attempt. A line the base's copy of a gate printed (said with
    that prefix, security-gate.py's base_copy_verdict) is not the gate's own."""
    found = {}
    for name in BASE_GATES:
        job = jobs.get(name) or {}
        text = api.text("projects/%s/jobs/%s/trace" % (project, job["id"])) if job.get("id") else ""
        found[name] = [line for line in (text or "").splitlines() if "base's copy:" not in line]
    return found


def gate_bases(logs):
    """{gate: the base its log says, the last it says, or None} from gate_logs."""
    found = {}
    for name, lines in logs.items():
        said = None
        for line in lines:
            match = BASE_LINE.search(line)
            if match:
                said = match.group(1)
        found[name] = said
    return found


def base_copies_missing(logs, changed):
    """[(gate, its script)] for each gate of BASE_COPY_SCRIPTS whose log does not say that
    the base's copy of its script passed the batch, or that the base has none, when the
    batch changes GATE_CODE; [] when it does not."""
    if not any(p in GATE_CODE for p in changed):
        return []
    missing = []
    for name, script in sorted(BASE_COPY_SCRIPTS.items()):
        # The line ends a line of the log, which may begin with a time GitLab gives it.
        said = [line.strip() for line in logs.get(name, [])]
        if not any(line.endswith(BASE_COPY_RAN % script) or line.endswith(BASE_COPY_NONE % script) for line in said):
            missing.append((name, script))
    return missing


def describe_jobs(statuses):
    return ", ".join("%s %s%s" % (name, status, " while allowed to fail" if allowed and status != "success" else "")
                     for name, status, allowed in statuses)


def land(opts, git, api, out, sleep=time.sleep, clock=time.time, scan=secret_scan, checkout=checkout_differs):
    say = lambda text="": print(text, file=out)
    branch = opts.branch
    if not BRANCH.match(branch or ""):
        raise Refused(2, "%r is not a batch* branch: only a protected batch* branch runs the gate a landing waits for" % branch)
    tip = git.sha("refs/heads/" + branch)
    if not tip:
        raise Refused(2, "there is no local branch %s" % branch)
    remote = opts.remote or detect_remote(git)
    project = opts.project

    # 1. What can be checked here, before anything leaves the machine.
    git.run("fetch", "--quiet", remote, "+refs/heads/%s:refs/remotes/%s/%s" % (TARGET, remote, TARGET))
    base = git.sha("refs/remotes/%s/%s" % (remote, TARGET))
    if not base:
        raise Refused(2, "%s/%s cannot be read" % (remote, TARGET))
    # What judges the batch here is this checkout's: it must be the base's, unedited - or, on
    # --own-copies, for a batch that repairs what master's copies cannot land, the batch's
    # own, every one as its tip has it.
    stale = checkout(git.repo, base)
    own_copies = (getattr(opts, "own_copies", None) or "").strip()
    if stale and not own_copies:
        raise Refused(2, "REFUSED: this landing runs from %s, whose %s %s not as %s/%s (%s) has %s, so the batch would be "
                         "judged by other code than %s's: bring that checkout to %s, with nothing in %s edited, and run "
                         "this again from it - or, for a batch that repairs what %s's copies cannot land, run it from the "
                         "batch's own worktree with --own-copies \"<who, why>\" (docs/ContinuousIntegration.md, \"Landing "
                         "on master\")" % (ROOT, ", ".join(stale), "is" if len(stale) == 1 else "are", remote, TARGET,
                                           base[:12], "it" if len(stale) == 1 else "them", TARGET, base[:12],
                                           "it" if len(stale) == 1 else "them", TARGET))
    if stale:
        # The batch's own copies, then, and nothing else: every one as its tip has it, so
        # that a checkout behind master, an edited one, or one that has master's copy of a
        # file the batch changes beside the batch's of another, is refused all the same.
        mixed = checkout(git.repo, tip)
        if mixed:
            raise Refused(2, "REFUSED: --own-copies lands a batch only from its own worktree, every file a landing runs as "
                             "the batch's tip has it, and this landing runs from %s, whose %s %s not as %s (%s) has %s: "
                             "bring that checkout to %s, with nothing in %s edited, and run this again from it"
                             % (ROOT, ", ".join(mixed), "is" if len(mixed) == 1 else "are", branch, tip[:12],
                                "it" if len(mixed) == 1 else "them", tip[:12], "it" if len(mixed) == 1 else "them"))
        checkout_line = ("the batch's own, not %s's (%s), on --own-copies: %s - the batch was judged by its own copies "
                         "of what judges it" % (TARGET, ", ".join(stale), own_copies))
        say("WARNING: this landing runs from the batch's own copies of %s, not %s/%s's, on --own-copies: %s. The batch is "
            "judged by its own copies of what judges it, and the landing record keeps the reason."
            % (", ".join(stale), remote, TARGET, own_copies))
    else:
        checkout_line = "%s's" % TARGET
        if own_copies:
            checkout_line += " (--own-copies was given, and not needed: %s)" % own_copies
            say("--own-copies is not needed: this checkout's copies of the files a landing runs are %s/%s's." % (remote, TARGET))
    # The sign-off check is one of those copies: loaded, which runs its code, only now.
    sign_off = _load_sign_off()
    if not git.ok("merge-base", "--is-ancestor", base, tip):
        raise Refused(3, "REFUSED: %s (%s) is not on top of %s/%s (%s); the project merges by fast-forward only. "
                         "Rebase it onto %s/%s; its pipeline then runs again." % (branch, tip[:12], remote, TARGET, base[:12], remote, TARGET))
    shas = [s for s in git.run("rev-list", "--reverse", "%s..%s" % (base, tip)).split() if s]
    if not shas:
        raise Refused(3, "REFUSED: %s adds nothing to %s/%s" % (branch, remote, TARGET))
    merges = [s for s in git.run("rev-list", "--merges", "%s..%s" % (base, tip)).split() if s]
    if merges:
        raise Refused(3, "REFUSED: %s adds %d merge commit(s) (%s); master's history is linear" % (branch, len(merges), ", ".join(m[:12] for m in merges)))
    exempt = release_tags(git, remote)
    missing, looked = sign_off.unsigned(git.repo, base, tip, exempt)
    if missing:
        for sha, subject, author in missing:
            say("MISSING  %s %s - no Signed-off-by for %s" % (sha[:12], subject[:90], author))
        raise Refused(4, "REFUSED: %d of the %d commit(s) %s adds lack their author's Signed-off-by. %s"
                         % (len(missing), looked, branch, sign_off.FIX.replace("<base>", "%s/%s" % (remote, TARGET))))
    # The secret scan of every commit the batch brings to master, before anything is
    # pushed: a possible secret, or a scan that did not run, refuses (a scan that did
    # not run has not said anything is clean).
    scanned, report = scan(git.repo, base, tip)
    for line in report.splitlines():
        say(line)
    if scanned != 0:
        why = {1: "found a possible secret", 3: "had no scanner to run"}.get(scanned, "failed (exit %d)" % scanned)
        raise Refused(8, "REFUSED: the secret scan of the %d commit(s) %s adds to %s/%s %s, so nothing was pushed. "
                         "A real secret is revoked and taken out of the commits; a fingerprint that is not one goes "
                         "in .gitleaksignore (CONTRIBUTING.md, \"Secrets\")." % (len(shas), branch, remote, TARGET, why))
    # A batch that changes the gates' own files changes what judges it: a person reads
    # that change whole, and says so (--gates-read), before anything is pushed.
    changed = [p for p in git.run("diff", "--name-only", "--no-renames", "-z", base, tip).split("\0") if p]
    gate_files = [p for p in changed if p in GATE_FILES]
    gates_read = (getattr(opts, "gates_read", None) or "").strip()
    if gate_files and not gates_read:
        raise Refused(9, "REFUSED: %s changes the gates' own files (%s). The base's copy of the gate scripts judges it too, "
                         "but only a person reads such a change whole: read it, then run this again with --gates-read "
                         "REASON, which the landing record keeps (SECURITY.md, \"What the gates cannot hold\")"
                         % (branch, ", ".join(gate_files)))
    authors = sorted(set(tuple(line.split("\x1f", 1)) for line in
                         git.run("log", "--format=%an\x1f%ae", "%s..%s" % (base, tip)).splitlines() if "\x1f" in line))
    say("%s at %s: %d commit(s) on top of %s/%s %s, every one signed off%s, and no possible secret in them." % (
        branch, tip[:12], len(shas), remote, TARGET, base[:12],
        " (%d already in a release tag not looked at)" % (len(shas) - looked) if looked != len(shas) else ""))
    if opts.dry_run:
        say("Dry run: would push %s to %s, open or find its merge request into %s, wait for the pipeline of %s, "
            "and merge it by fast-forward." % (branch, remote, TARGET, tip[:12]))
        return 0

    # 2. The push.
    code, out_ls = git.run("ls-remote", "--heads", remote, "refs/heads/" + branch, check=False)
    remote_sha = out_ls.split()[0] if code == 0 and out_ls.strip() else None
    if remote_sha == tip:
        say("%s/%s is already at %s." % (remote, branch, tip[:12]))
        on_remote_since = "this run found %s/%s at it" % (remote, branch)
    else:
        if remote_sha:
            git.run("fetch", "--quiet", remote, "+refs/heads/%s:refs/remotes/%s/%s" % (branch, remote, branch))
        if remote_sha and not git.ok("merge-base", "--is-ancestor", remote_sha, tip):
            if not opts.force_push:
                raise Refused(3, "REFUSED: %s/%s (%s) holds commits %s does not; --force-push replaces it"
                                 % (remote, branch, remote_sha[:12], branch))
            git.run("push", "--quiet", "--force-with-lease=refs/heads/%s:%s" % (branch, remote_sha), remote, "%s:refs/heads/%s" % (tip, branch))
        else:
            git.run("push", "--quiet", remote, "%s:refs/heads/%s" % (tip, branch))
        say("Pushed %s to %s/%s." % (tip[:12], remote, branch))
        on_remote_since = "the push to %s/%s" % (remote, branch)
    # From here the branch on GitLab holds the tip: the merge request's head follows it a
    # moment later (HEAD_WAIT_SECONDS).
    on_remote_at = clock()

    # 3. The merge request.
    found = api("GET", "projects/%s/merge_requests?state=opened&source_branch=%s&target_branch=%s" % (project, q(branch), TARGET)) or []
    if found:
        mr = found[0]
        say("Merge request !%d is open for %s: %s" % (mr["iid"], branch, mr.get("web_url", "")))
    else:
        lines = [s for s in git.run("log", "--reverse", "--format=- %h %s", "%s..%s" % (base, tip)).splitlines()]
        description = "\n".join([
            "Landing %s: %d commit(s) on top of %s, opened by `build/land-via-mr.py`." % (branch, len(shas), base[:12]),
            "",
            "It merges by fast-forward once this branch's pipeline has passed, and each gate job in it "
            "(`%s`), and every thread is resolved. " % "`, `".join(GATE_JOBS + SOURCE_SCAN_JOBS + WINDOWS_GATE_JOBS) +
            "Every commit carries its author's `Signed-off-by`. A second person's review is an approval: "
            "read the change, then press Approve, or open a thread for what must change "
            "(GOVERNANCE.md, \"Review\").",
            "",
            "Commits:",
            "",
        ] + [l[:250] for l in lines[:200]] + (["- ... and %d more" % (len(lines) - 200)] if len(lines) > 200 else []))
        mr = api("POST", "projects/%s/merge_requests" % project, {
            "source_branch": branch, "target_branch": TARGET,
            "title": "Land %s: %d commit(s)" % (branch, len(shas)),
            "description": description, "remove_source_branch": True, "squash": False})
        say("Opened merge request !%d: %s" % (mr["iid"], mr.get("web_url", "")))
    iid = mr["iid"]
    if opts.no_wait:
        say("Not waiting (--no-wait). Run this again to wait for the pipeline and land.")
        return 0

    # 4. The pipeline of exactly this tip, once the merge request's head is the tip, which
    # GitLab makes it a moment after the push (HEAD_WAIT_SECONDS).
    deadline = clock() + opts.timeout_hours * 3600
    head_deadline = on_remote_at + HEAD_WAIT_SECONDS
    head_seen = False
    head_waiting = False
    last = None
    while True:
        mr = api("GET", "projects/%s/merge_requests/%d" % (project, iid))
        if mr.get("state") != "opened":
            raise Refused(5, "REFUSED: merge request !%d is %s" % (iid, mr.get("state")))
        head = mr.get("sha")
        head_pipeline_sha = (mr.get("head_pipeline") or {}).get("sha")
        head_pipeline_said = head_pipeline_sha[:12] if head_pipeline_sha else "none"
        if head and head != tip:
            if head_seen:
                # It was the tip, and is not now: the branch was pushed again while this waited.
                raise Refused(5, "REFUSED: merge request !%d's head is %s, not the %s checked here; run this again"
                                 % (iid, head[:12], tip[:12]))
            if clock() >= head_deadline:
                raise Refused(5, "REFUSED: merge request !%d's head is still %s (its head pipeline's commit: %s), not the "
                                 "%s checked here, %d minute(s) after %s: GitLab has not moved the merge request to it; "
                                 "run this again" % (iid, head[:12], head_pipeline_said, tip[:12], HEAD_WAIT_SECONDS // 60,
                                                     on_remote_since))
            if not head_waiting:
                head_waiting = True
                say("Merge request !%d's head is %s (its head pipeline's commit: %s), not yet %s: GitLab moves it a moment "
                    "after %s. Waiting for it to be %s, up to %d minute(s) from then, reading it every %d s."
                    % (iid, head[:12], head_pipeline_said, tip[:12], on_remote_since, tip[:12], HEAD_WAIT_SECONDS // 60,
                       HEAD_POLL_SECONDS))
            sleep(HEAD_POLL_SECONDS)
            continue
        if head == tip and not head_seen:
            head_seen = True
            if head_waiting:
                say("Merge request !%d's head is %s, %d s after %s; its head pipeline's commit: %s%s." % (
                    iid, tip[:12], int(clock() - on_remote_at), on_remote_since, head_pipeline_said,
                    "" if head_pipeline_sha == tip else " (the pipeline of %s is looked for by its commit)" % tip[:12]))
        pipeline = mr.get("head_pipeline")
        if not pipeline or pipeline.get("sha") != tip:
            listed = api("GET", "projects/%s/pipelines?ref=%s&sha=%s&order_by=id&sort=desc" % (project, q(branch), tip)) or []
            pipeline = listed[0] if listed else None
        status = pipeline.get("status") if pipeline else "not yet created"
        if status != last:
            say("Pipeline %s: %s" % ("#%s %s" % (pipeline["id"], pipeline.get("web_url", "")) if pipeline else "of %s" % tip[:12], status))
            last = status
        if status == "success":
            # The project's own pipeline: one a fork ran, on runners its owner chose, is
            # that owner's to report.
            if str(pipeline.get("project_id", "")) != str(project):
                raise Refused(5, "REFUSED: pipeline #%s ran in project %s, not this one (%s): a pipeline another project "
                                 "ran is not the gate (SECURITY.md, 'Every change')" % (
                                     pipeline["id"], pipeline.get("project_id", "unknown"), project))
            break
        if pipeline and status not in PENDING:
            raise Refused(5, "REFUSED: the pipeline of %s is %s (%s)" % (tip[:12], status, pipeline.get("web_url", "")))
        if clock() > deadline:
            raise Refused(5, "REFUSED: no answer from the pipeline of %s in %s hour(s); run this again to go on waiting"
                             % (tip[:12], opts.timeout_hours))
        sleep(opts.poll_seconds)

    # ... and the gates in it, each of which must have succeeded itself: GitLab calls a
    # pipeline a success with a job that failed while allowed to fail - a half of the
    # Windows suite a busy bench let pass among them - or that a rule never created (a
    # push of documents alone trims the scans).
    jobs = pipeline_jobs(api, project, pipeline["id"])
    statuses = required_jobs(jobs, changed)
    jobs_line = describe_jobs(statuses)
    if documents_only(changed):
        let_off = SOURCE_SCAN_JOBS + WINDOWS_GATE_JOBS
        jobs_line += " (%s and %s not required: the batch changes Markdown alone, at the root or under a dot-directory)" % (
            ", ".join(let_off[:-1]), let_off[-1])
    say("Gate jobs of pipeline #%s: %s" % (pipeline["id"], jobs_line))
    hosted = [(name, jobs[name].get("status") or "unknown", bool(jobs[name].get("allow_failure")))
              for name in HOSTED_CHECK_JOBS if name in jobs]
    hosted_line = describe_jobs(hosted) if hosted else "not in the pipeline (no switch or lane was set)"
    say("GitLab's hosted runners' checks of pipeline #%s: %s" % (pipeline["id"], hosted_line))
    held = [(name, status, allowed) for name, status, allowed in statuses if status != "success"]
    if held:
        raise Refused(5, "REFUSED: pipeline #%s of %s succeeded, but %s did not: %s. A gate that a rule left out of the "
                         "pipeline, that was skipped, or that failed while allowed to fail has not held the batch; push "
                         "the tip again so that its pipeline runs them, or find why it did not - a half of the Windows "
                         "suite that a busy bench let pass (exit 10) is gated by retrying its job, and one a push of "
                         "documents alone left out by a pipeline of the branch started by hand, which counts every file "
                         "as changed (docs/ContinuousIntegration.md, "
                         "\"Landing on master\")." % (pipeline["id"], tip[:12], " and ".join(n for n, _, _ in held),
                                                      describe_jobs(held)))
    # ... and against the base the batch leaves master at: a gate whose log names another
    # judged another change than the one that lands (a CI file can stand in for GitLab's
    # variables).
    logs = gate_logs(api, project, jobs)
    bases = gate_bases(logs)
    astray = ["%s %s" % (name, ("judged it against %s" % said[:12]) if said else "says no base")
              for name, said in sorted(bases.items()) if said != base]
    if astray:
        raise Refused(5, "REFUSED: the gates of pipeline #%s did not judge %s against %s/%s at %s, where it leaves it: %s "
                         "(docs/ContinuousIntegration.md, \"Landing on master\")" % (
                             pipeline["id"], branch, remote, TARGET, base[:12], "; ".join(astray)))
    say("Each of %s judged the batch against %s, where it leaves %s/%s." % (" and ".join(BASE_GATES), base[:12], remote, TARGET))
    # ... and, when the batch changes the gates' code, by master's copy of it as well.
    unjudged = base_copies_missing(logs, changed)
    if unjudged:
        raise Refused(5, "REFUSED: %s changes the gates' code, and the log of %s in pipeline #%s does not say that %s's "
                         "copy of %s judged it and passed it (\"%s\"): the gate's own code decides whether that copy runs, "
                         "so its log must show that it did (docs/ContinuousIntegration.md, \"Landing on master\")" % (
                             branch, " and ".join(n for n, _ in unjudged), pipeline["id"], TARGET,
                             " and ".join(s for _, s in unjudged), BASE_COPY_RAN % unjudged[0][1]))
    if any(p in GATE_CODE for p in changed):
        copies_line = "judged the batch and passed it, as %s's logs say" % " and ".join(sorted(BASE_COPY_SCRIPTS))
        say("Master's copy of the gates' code judged the batch too, and passed it, as %s's logs say."
            % " and ".join(sorted(BASE_COPY_SCRIPTS)))
    else:
        copies_line = "not needed: the batch leaves the gates' code as master has it"

    # 5. GitLab's own answer.
    settle = clock() + 900
    while True:
        mr = api("GET", "projects/%s/merge_requests/%d" % (project, iid))
        status = mr.get("detailed_merge_status") or mr.get("merge_status")
        if status in ("mergeable", "can_be_merged"):
            break
        if status not in SETTLING or clock() > settle:
            raise Refused(6, "REFUSED: GitLab says merge request !%d is %s, not mergeable (%s)" % (iid, status, mr.get("web_url", "")))
        sleep(min(opts.poll_seconds, 20))

    # 6. Approvals.
    approvers, source = approvals(api, project, iid)
    arrived = tip_arrived(api, project, iid, tip)
    on_tip, earlier = current_approvals(approvers, arrived)
    mr_author = (mr.get("author") or {}).get("username", "")
    second = second_people(on_tip, mr_author, authors)
    stale = second_people(earlier, mr_author, authors)
    if second:
        verdict = "yes: " + ", ".join("@%s (%s)" % (a["username"], a.get("name", "")) for a in second)
    elif stale:
        verdict = "NO. Approved by %s before %s reached the merge request, not since." % (
            ", ".join("@" + a["username"] for a in stale), tip[:12])
    else:
        verdict = "NO. " + ("Approved only by the author's own account(s): %s." % ", ".join("@" + a["username"] for a in on_tip)
                            if on_tip else "Nobody approved.")
    if arrived is None:
        verdict += " (When %s reached the merge request could not be read, so an approval was counted whatever it was given on.)" % tip[:12]
    say("Second-person approval (read from %s): %s" % (source, verdict))
    override = (getattr(opts, "land_without_approval", None) or "").strip()
    try:
        lander = (api("GET", "user") or {}).get("username", "")
    except ApiError:
        lander = ""
    pipeline_line = "#%s %s, %s" % (pipeline["id"], pipeline.get("status"), pipeline.get("web_url", ""))
    signoff_line = "every commit (%d) carries its author's Signed-off-by%s" % (
        len(shas), "; %d already in a release tag not looked at" % (len(shas) - looked) if looked != len(shas) else "")

    def record(outcome):
        now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
        body = "\n".join([
            "**Landing record** (`build/land-via-mr.py`, %s)" % now,
            "",
            "- Outcome: %s" % outcome,
            "- Tip: `%s`, %d commit(s) on top of master `%s`" % (tip, len(shas), base[:12]),
            "- Pipeline: %s" % pipeline_line,
            "- Gate jobs: %s" % jobs_line,
            "- GitLab's hosted runners' checks: %s" % hosted_line,
            "- The gates' base: `%s`, where the batch leaves master, as %s said" % (base[:12], " and ".join(BASE_GATES)),
            "- The gates' own files: %s" % (("changed (%s), read: %s" % (", ".join(gate_files), gates_read))
                                            if gate_files else "not changed"),
            "- The checkout's copies of what the landing runs: %s" % checkout_line,
            "- Master's copy of the gates' code: %s" % copies_line,
            "- Sign-off: %s" % signoff_line,
            "- Second-person approval (from %s): %s%s" % (
                source, verdict, "" if second else
                (" Landed WITHOUT it, on the owner's override (--land-without-approval): %s. To be reviewed afterwards "
                 "(GOVERNANCE.md, \"Review\")." % override if override and lander.lower() in OWNER else
                 " It is required (GOVERNANCE.md, \"Review\").")),
            "- Landed by: @%s" % (lander or "unknown"),
        ])
        try:
            api("POST", "projects/%s/merge_requests/%d/notes" % (project, iid), {"body": body})
        except ApiError as e:
            say("The landing record could not be posted on !%d: %s" % (iid, e))
        try:
            common = git.run("rev-parse", "--git-common-dir").strip()
            common = common if os.path.isabs(common) else os.path.join(git.repo, common)
            with io.open(os.path.join(common, "land-via-mr.log"), "a", encoding="utf-8") as log:
                log.write("%s\t%s\t!%d\t%s\t%s\tpipeline %s\tgate jobs: %s\tsecond-person approval: %s\t"
                          "the checkout's copies: %s\n" % (now, branch, iid, tip, outcome, pipeline_line, jobs_line, verdict,
                                                           checkout_line))
        except (IOError, OSError, Refused):
            pass

    if not second:
        if not override:
            record("not merged: no second person approved, and one is required")
            raise Refused(7, "REFUSED: no second person approved the tip of merge request !%d. Every merge request needs "
                             "Zain Ul Abidin's approval (the owner's, for Zain's own), given once the tip it lands had "
                             "reached it (GOVERNANCE.md, \"Review\"); the owner alone may override that with "
                             "--land-without-approval REASON" % iid)
        if lander.lower() not in OWNER:
            record("not merged: --land-without-approval was given by @%s, and only the owner may override the approval rule"
                   % (lander or "unknown"))
            raise Refused(7, "REFUSED: --land-without-approval is the owner's alone, and this runs as @%s"
                             % (lander or "an account that could not be read"))
        say("WARNING: no second person approved merge request !%d. It lands on the owner's override: %s. "
            "Have it reviewed afterwards (GOVERNANCE.md, \"Review\")." % (iid, override))

    # 7. The merge, by fast-forward, at exactly this tip.
    try:
        merged = api("PUT", "projects/%s/merge_requests/%d/merge" % (project, iid),
                     {"sha": tip, "squash": False, "should_remove_source_branch": True})
    except ApiError as e:
        record("not merged: GitLab refused the merge (%s)" % str(e)[:200])
        raise Refused(6, "REFUSED: GitLab would not merge !%d: %s" % (iid, e))
    if (merged or {}).get("state") != "merged":
        record("not merged: GitLab answered %s" % (merged or {}).get("state"))
        raise Refused(6, "REFUSED: merge request !%d is %s after the merge call" % (iid, (merged or {}).get("state")))
    git.run("fetch", "--quiet", remote, "+refs/heads/%s:refs/remotes/%s/%s" % (TARGET, remote, TARGET))
    now_master = git.sha("refs/remotes/%s/%s" % (remote, TARGET))
    if now_master != tip:
        record("merged, but %s/%s is %s, not the tip" % (remote, TARGET, (now_master or "")[:12]))
        raise Refused(2, "merge request !%d says merged, but %s/%s is %s, not %s" % (iid, remote, TARGET, (now_master or "?")[:12], tip[:12]))
    record("merged by fast-forward; master is %s" % tip[:12])
    say("Landed: %s/%s is %s (merge request !%d)." % (remote, TARGET, tip[:12], iid))

    # 8. Local refs and the branch.
    worktrees = git.run("worktree", "list", "--porcelain")
    local_master = git.sha("refs/heads/" + TARGET)
    if "branch refs/heads/%s\n" % TARGET in worktrees + "\n":
        where = [block.split("\n", 1)[0][len("worktree "):] for block in worktrees.replace("\r\n", "\n").split("\n\n")
                 if block.startswith("worktree ") and "\nbranch refs/heads/%s\n" % TARGET in block + "\n"]
        say("A worktree has %s checked out (%s); the local %s was left as it is. Bring it to %s (git -C <that worktree> "
            "merge --ff-only %s) before the next landing, which refuses a checkout whose copies of the files it runs are "
            "not %s's." % (TARGET, ", ".join(where) or "?", TARGET, tip[:12], tip, TARGET))
    elif local_master is None or git.ok("merge-base", "--is-ancestor", local_master, tip):
        git.run("update-ref", "refs/heads/" + TARGET, tip)
        say("The local %s is %s." % (TARGET, tip[:12]))
    else:
        say("The local %s (%s) is not an ancestor of the tip; it was left as it is." % (TARGET, local_master[:12]))
    try:
        api("GET", "projects/%s/repository/branches/%s" % (project, q(branch)))
        try:
            api("DELETE", "projects/%s/repository/branches/%s" % (project, q(branch)))
            say("Deleted %s on GitLab." % branch)
        except ApiError as e:
            say("%s is still on GitLab and could not be deleted here (%s); delete it under Code, Branches." % (branch, str(e)[:160]))
    except ApiError:
        say("%s is gone from GitLab." % branch)
    if "branch refs/heads/%s\n" % branch in worktrees + "\n":
        say("A worktree has %s checked out; remove the worktree, then the branch." % branch)
    elif git.ok("branch", "-d", branch):
        say("Deleted the local branch %s." % branch)
    return 0


# ---------------------------------------------------------------- the self-test

def full_pipeline(**changes):
    """A batch pipeline's jobs as the jobs API lists them: 110 others first, so that the
    gates are on the second page, then the gates, each a success unless changes gives it
    (status, allow_failure), or None to leave it out."""
    jobs = [{"id": 1000 + n, "name": "job-%d" % n, "status": "success", "allow_failure": False} for n in range(110)]
    for n, name in enumerate(GATE_JOBS + SOURCE_SCAN_JOBS + WINDOWS_GATE_JOBS):
        if changes.get(name, ("success", False)) is None:
            continue
        status, allowed = changes.get(name, ("success", False))
        jobs.append({"id": 2000 + n, "name": name, "status": status, "allow_failure": allowed})
    return jobs


class FakeGitLab(object):
    """Enough of GitLab's API, in memory, over a bare repository standing for the project."""

    def __init__(self, bare, approved_by=None, pipeline_statuses=None, mergeable="mergeable", author="Progressiverobot",
                 approvals_api=True, protected_branch=False, approved_at="2026-09-26T11:00:00.000Z",
                 version_at="2026-09-26T10:00:00.000Z", versions_api=True, lander="Progressiverobot", jobs=None,
                 pipeline_project=1, gate_base=None, base_copy=BASE_COPY_RAN):
        self.bare = bare
        self.pipeline_project = pipeline_project
        # The base each gate's log says: master's tip when the log is read, unless given.
        self.gate_base = gate_base
        # What each gate's log says of the base's copy of its script, with the script put
        # in, or nothing (None).
        self.base_copy = base_copy
        self.lander = lander
        self.jobs = full_pipeline() if jobs is None else jobs
        self.mrs = {}
        self.notes = []
        self.calls = []
        self.approved_by = approved_by or []
        self.statuses = list(pipeline_statuses or ["running", "success"])
        self.mergeable = mergeable
        self.author = author
        self.approvals_api = approvals_api
        self.protected_branch = protected_branch
        self.approved_at = approved_at
        self.version_at = version_at
        self.versions_api = versions_api
        # GitLab a moment after a push: each read of a merge request takes the first of these,
        # (its head, its head pipeline's commit, that pipeline's status), None for each the
        # branch's own tip and the pipeline's status as it is; once they are gone, the reads
        # see the branch as it is.
        self.heads = []
        # A clock (the self-test's) that the search for the branch's open merge request moves
        # on by lookup_seconds, as a slow answer would; and how many reads of a merge request
        # a landing may make before the fake takes it for a loop that does not end.
        self.clock = None
        self.lookup_seconds = 0
        self.reads = 0
        self.most_reads = 500

    def _ref(self, ref):
        done = subprocess.run(["git", "--git-dir", self.bare, "rev-parse", "--verify", "--quiet", ref], stdout=subprocess.PIPE)
        return done.stdout.decode().strip() or None

    def __call__(self, method, path, fields=None):
        self.calls.append((method, path, dict(fields or {})))
        path_only, _, query = path.partition("?")
        params = dict(urllib.parse.parse_qsl(query))
        m = re.match(r"^projects/\d+/merge_requests$", path_only)
        if m and method == "GET":
            if self.clock is not None:
                self.clock.now += self.lookup_seconds
            return [mr for mr in self.mrs.values() if mr["state"] == "opened" and mr["source_branch"] == params.get("source_branch")]
        if m and method == "POST":
            iid = len(self.mrs) + 1
            branch = fields["source_branch"]
            self.mrs[iid] = {"iid": iid, "state": "opened", "source_branch": branch, "author": {"username": self.author},
                             "web_url": "https://example.invalid/mr/%d" % iid, "fields": dict(fields)}
            return self.mrs[iid]
        m = re.match(r"^projects/\d+/merge_requests/(\d+)$", path_only)
        if m and method == "GET":
            self.reads += 1
            if self.reads > self.most_reads:
                raise RuntimeError("the landing read merge request !%s %d times: a loop that does not end" % (
                    m.group(1), self.most_reads))
            mr = dict(self.mrs[int(m.group(1))])
            sha = self._ref("refs/heads/" + mr["source_branch"])
            status = self.statuses.pop(0) if len(self.statuses) > 1 else self.statuses[0]
            head, pipeline_sha = sha, sha
            if self.heads:
                said_head, said_pipeline, said_status = self.heads.pop(0)
                head, pipeline_sha, status = said_head or sha, said_pipeline or sha, said_status or status
            mr["sha"] = head
            # The tip's pipeline is #7, whose jobs these are; an earlier commit's is #6.
            number = 7 if pipeline_sha == sha else 6
            mr["head_pipeline"] = {"id": number, "sha": pipeline_sha, "status": status,
                                   "web_url": "https://example.invalid/p/%d" % number, "project_id": self.pipeline_project}
            mr["detailed_merge_status"] = self.mergeable
            return mr
        m = re.match(r"^projects/\d+/merge_requests/(\d+)/approvals$", path_only)
        if m:
            if not self.approvals_api:
                raise ApiError("403 Forbidden")
            return {"approved_by": [{"user": dict(a), "approved_at": self.approved_at} for a in self.approved_by]}
        m = re.match(r"^projects/\d+/merge_requests/(\d+)/versions$", path_only)
        if m:
            if not self.versions_api:
                raise ApiError("404 Not Found")
            sha = self._ref("refs/heads/" + self.mrs[int(m.group(1))]["source_branch"])
            return [{"id": 2, "head_commit_sha": sha, "created_at": self.version_at},
                    {"id": 1, "head_commit_sha": "0" * 40, "created_at": "2026-09-25T09:00:00.000Z"}]
        m = re.match(r"^projects/\d+/merge_requests/(\d+)/notes$", path_only)
        if m and method == "GET":
            return [{"system": True, "body": "approved this merge request", "author": dict(a), "created_at": self.approved_at}
                    for a in self.approved_by] + \
                   [{"system": True, "body": "unapproved this merge request", "author": {"username": "withdrawn", "name": "W"}},
                    {"system": False, "body": "approved this merge request", "author": {"username": "joker", "name": "J"}}]
        if m and method == "POST":
            self.notes.append(fields["body"])
            return {"id": len(self.notes)}
        m = re.match(r"^projects/\d+/merge_requests/(\d+)/merge$", path_only)
        if m and method == "PUT":
            mr = self.mrs[int(m.group(1))]
            branch_sha = self._ref("refs/heads/" + mr["source_branch"])
            if fields.get("sha") != branch_sha or fields.get("squash") is not False:
                raise ApiError("409 SHA does not match HEAD of source branch, or a squash was asked for")
            master = self._ref("refs/heads/master")
            ok = subprocess.run(["git", "--git-dir", self.bare, "merge-base", "--is-ancestor", master, branch_sha]).returncode == 0
            if not ok:
                raise ApiError("406 Branch cannot be merged")
            subprocess.run(["git", "--git-dir", self.bare, "update-ref", "refs/heads/master", branch_sha], check=True)
            if fields.get("should_remove_source_branch") and not self.protected_branch:
                subprocess.run(["git", "--git-dir", self.bare, "update-ref", "-d", "refs/heads/" + mr["source_branch"]], check=True)
            mr["state"] = "merged"
            return dict(mr)
        m = re.match(r"^projects/\d+/repository/branches/(.+)$", path_only)
        if m:
            name = urllib.parse.unquote(m.group(1))
            if method == "GET":
                if self._ref("refs/heads/" + name):
                    return {"name": name}
                raise ApiError("404 Branch Not Found")
            if method == "DELETE":
                if self.protected_branch:
                    raise ApiError("403 Forbidden")
                subprocess.run(["git", "--git-dir", self.bare, "update-ref", "-d", "refs/heads/" + name], check=True)
                return None
        m = re.match(r"^projects/\d+/pipelines/(\d+)/jobs$", path_only)
        if m and method == "GET":
            size, page = int(params.get("per_page", "20")), int(params.get("page", "1"))
            return [dict(j) for j in self.jobs[(page - 1) * size:page * size]]
        if path_only == "projects/1/pipelines" or re.match(r"^projects/\d+/pipelines$", path_only):
            return []
        if path_only == "user":
            return {"username": self.lander}
        raise ApiError("the fake has no %s %s" % (method, path))

    def text(self, path):
        """A job's log: a gate's says its base, as security-gate.py's auto_base says it, and
        the base's copy's line, prefixed, names another that is not the gate's own."""
        self.calls.append(("GET", path, {}))
        m = re.match(r"^projects/\d+/jobs/(\d+)/trace$", path)
        if not m:
            raise ApiError("the fake has no log at %s" % path)
        said = self.gate_base if self.gate_base is not None else self._ref("refs/heads/master")
        name = next((j["name"] for j in self.jobs if str(j["id"]) == m.group(1)), "")
        lines = ["Running with gitlab-runner", "  base's copy: The base: %s, a line of the base's copy." % ("e" * 40)]
        if said:
            lines.append("The base: %s, where HEAD leaves master: git merge-base HEAD refs/remotes/origin/master." % said)
        if self.base_copy and name in BASE_COPY_SCRIPTS:
            lines.append(self.base_copy % BASE_COPY_SCRIPTS[name])
        return "\n".join(lines + ["Job succeeded"]) + "\n"


def self_test():
    failures = []

    def expect(what, got, want):
        if got != want:
            failures.append("%s: got %r, want %r" % (what, got, want))

    # The second-person rule, alone.
    zain = {"username": "zainulabidin1990", "name": "Zain Ul Abidin"}
    chris_old = {"username": "chrisholloway5", "name": "Christopher Holloway"}
    owner_commits = [("Christopher Holloway", "chris.holloway@progressiverobot.com")]
    expect("the other maintainer counts", second_people([zain], "Progressiverobot", owner_commits), [zain])
    expect("the author's older account does not", second_people([chris_old], "Progressiverobot", owner_commits), [])
    expect("nor under another display name, by the table of one person's accounts",
           second_people([{"username": "chrisholloway5", "name": "C H"}], "Progressiverobot", [("Someone", "someone@example.org")]), [])
    expect("nor by name, under a username the table lacks",
           second_people([{"username": "someone", "name": "christopher holloway"}], "someone-else", owner_commits), [])
    expect("an author of a commit does not, though another opened the merge request",
           second_people([zain], "Progressiverobot", [("Zain Ul Abidin", "zain@example.org")]), [])
    expect("the owner counts on a contributor's merge request",
           second_people([{"username": "Progressiverobot", "name": "Christopher Holloway"}], "contributor", [("Con", "con@example.org")]),
           [{"username": "Progressiverobot", "name": "Christopher Holloway"}])

    tmp = tempfile.mkdtemp(prefix="land-via-mr-")
    env_keep = {k: os.environ.get(k) for k in ("GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_GLOBAL", "HOME")}
    try:
        os.environ["GIT_CONFIG_NOSYSTEM"] = "1"
        os.environ["GIT_CONFIG_GLOBAL"] = os.path.join(tmp, "gitconfig")
        os.environ["HOME"] = tmp
        with io.open(os.environ["GIT_CONFIG_GLOBAL"], "w") as f:
            f.write("[user]\n\tname = Christopher Holloway\n\temail = chris.holloway@progressiverobot.com\n"
                    "[init]\n\tdefaultBranch = master\n[advice]\n\tdetachedHead = false\n")

        # The requests themselves, through a stand-in for glab that writes down the command
        # line it was started with and what it read on its standard input - only when it was
        # told to read it (--input), as glab does - and answers as GitLab would.
        stand_in = os.path.join(tmp, "glab-stand-in.py")
        seen_at = os.path.join(tmp, "glab-seen.json")
        with io.open(stand_in, "w", encoding="utf-8", newline="\n") as f:
            f.write("import io, json, sys\n"
                    "seen, args = sys.argv[1], sys.argv[2:]\n"
                    "body = sys.stdin.buffer.read() if '--input' in args else None\n"
                    "with io.open(seen, 'w', encoding='utf-8') as f:\n"
                    "    json.dump({'argv': args, 'body': None if body is None else body.decode('utf-8')}, f)\n"
                    "if any('refused' in a for a in args):\n"
                    "    sys.stderr.write('HTTP 403: 403 Forbidden\\n')\n"
                    "    sys.exit(1)\n"
                    "sys.stdout.write(json.dumps({'iid': 7, 'web_url': 'https://example.invalid/mr/7'}))\n")
        glab = Glab("gitlab.example", program=[sys.executable, stand_in, seen_at])

        def sent():
            with io.open(seen_at, encoding="utf-8") as f:
                return json.load(f)

        # batch47's description was 50,565 characters; this one is 60,000, and not ASCII.
        description = ("- 1df1dc3e7 A SEARCH asks each of its criteria of a message once \u2014 \u00e9t\u00e9\n" * 2000)[:60000]
        expect("the description is over Windows' command line as one -f argument, as it was sent",
               len("description=" + description) > 32767, True)
        fields = {"source_branch": "batch48", "target_branch": "master", "title": "Land batch48: 250 commit(s)",
                  "description": description, "remove_source_branch": True, "squash": False, "milestone_id": 12}
        try:
            answer = glab("POST", "projects/86730559/merge_requests", fields)
        except OSError as error:
            # Windows' "[WinError 206] The filename or extension is too long", as batch47's landing met it.
            raise RuntimeError("a 60,000-character description could not be sent: %s" % error)
        got = sent()
        expect("a 60,000-character description: GitLab's answer is read", answer, {"iid": 7, "web_url": "https://example.invalid/mr/7"})
        expect("a 60,000-character description: the command line holds no field",
               any(description[:40] in a or a.startswith(("description=", "title=")) for a in got["argv"]), False)
        expect("a 60,000-character description: the command line is short",
               sum(len(a) + 1 for a in [sys.executable, stand_in, seen_at] + got["argv"]) < 1000, True)
        expect("a 60,000-character description: the body goes on standard input as JSON",
               got["argv"][-4:], ["-H", "Content-Type: application/json", "--input", "-"])
        body = json.loads(got["body"] or "null")
        expect("a 60,000-character description: every field arrives, the description whole", body, fields)
        expect("a 60,000-character description: booleans stay booleans and an integer a number",
               [type(body.get(k)).__name__ for k in ("remove_source_branch", "squash", "milestone_id")], ["bool", "bool", "int"])
        record = "**Landing record** (`build/land-via-mr.py`)\n\n" + description
        glab("POST", "projects/86730559/merge_requests/7/notes", {"body": record})
        expect("the landing record goes the same way", (sent()["argv"][-1], json.loads(sent()["body"])), ("-", {"body": record}))
        glab("PUT", "projects/86730559/merge_requests/7/merge", {"sha": "a" * 40, "squash": False, "should_remove_source_branch": True})
        expect("the merge's fields, typed", json.loads(sent()["body"]),
               {"sha": "a" * 40, "squash": False, "should_remove_source_branch": True})
        glab("GET", "projects/86730559/merge_requests/7")
        expect("a GET without fields: no body, and glab is not told to read one", (sent()["body"], "--input" in sent()["argv"]),
               (None, False))
        glab("GET", "projects/86730559/merge_requests", {"state": "opened", "per_page": 100, "wip": False})
        expect("a GET's fields stay query parameters, as -f and -F made them", (sent()["body"], sent()["argv"][-6:]),
               (None, ["-f", "state=opened", "-F", "per_page=100", "-F", "wip=false"]))
        try:
            glab("POST", "projects/86730559/refused", {"body": "x"})
            expect("a request glab fails is an ApiError", "no error", "ApiError")
        except ApiError as error:
            expect("a request glab fails is an ApiError naming what glab said", "403 Forbidden" in str(error), True)

        def run(cwd, *args):
            done = subprocess.run(["git", "-C", cwd] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if done.returncode != 0:
                raise RuntimeError("git %s: %s" % (args, done.stderr.decode("utf-8", "replace")))
            return done.stdout.decode("utf-8", "replace").strip()

        def scenario(name):
            """A project (bare) with master and a released tag, and a clone with batchN on top."""
            base = os.path.join(tmp, name)
            bare, work = os.path.join(base, "project.git"), os.path.join(base, "work")
            os.makedirs(base)
            subprocess.run(["git", "init", "-q", "--bare", bare], check=True)
            subprocess.run(["git", "init", "-q", work], check=True)
            with io.open(os.path.join(work, "a.txt"), "w") as f:
                f.write("one\n")
            run(work, "add", "a.txt")
            run(work, "commit", "-q", "-m", "released, not signed off")
            run(work, "tag", "v1.0.0")
            run(work, "remote", "add", "origin", bare)
            run(work, "push", "-q", "origin", "master", "v1.0.0")
            run(work, "checkout", "-q", "-b", "batch1")
            return bare, work

        def commit(work, message, signoff=True):
            with io.open(os.path.join(work, "a.txt"), "a") as f:
                f.write(message + "\n")
            run(work, "add", "a.txt")
            run(work, *(["commit", "-q", "-m", message] + (["-s"] if signoff else [])))
            return run(work, "rev-parse", "HEAD")

        def commit_files(work, files, message):
            for relative, content in files.items():
                path = os.path.join(work, *relative.split("/"))
                if not os.path.isdir(os.path.dirname(path)):
                    os.makedirs(os.path.dirname(path))
                with io.open(path, "w") as f:
                    f.write(content)
                run(work, "add", relative)
            run(work, "commit", "-q", "-s", "-m", message)
            return run(work, "rev-parse", "HEAD")

        # The secret scan stands in: scan-secrets.py needs Docker or gitleaks, which the
        # checkers job does not have (its own self-test covers it). Each call is kept.
        scans = []

        def clean_scan(repo, base, tip):
            scans.append((repo, base, tip))
            return 0, "scan-secrets: nothing found\n"

        # The checkout's copies of the files a landing runs stand in too: this tree's are not
        # the scenarios' (checkout_differs is tried on its own below). Each call is kept.
        checkouts = []

        def same_checkout(repo, base):
            checkouts.append((repo, base))
            return []

        class Clock(object):
            """A clock that moves only when land() sleeps: what it waited for, and how long, is read
            from it, and a wait of minutes takes none."""

            def __init__(self):
                self.now = 1760000000.0
                self.slept = []

            def time(self):
                return self.now

            def sleep(self, seconds):
                self.slept.append(seconds)
                self.now += seconds

        def attempt(work, api, **kw):
            opts = argparse.Namespace(branch=kw.get("branch", "batch1"), remote="origin", project="1", dry_run=kw.get("dry_run", False),
                                      no_wait=kw.get("no_wait", False), force_push=False, timeout_hours=1, poll_seconds=0,
                                      land_without_approval=kw.get("override"), gates_read=kw.get("gates_read"),
                                      own_copies=kw.get("own_copies"))
            # The branch is left checked out in no worktree, as a batch is folded in its own.
            run(work, "checkout", "-q", "--detach")
            sink = io.StringIO()
            # checkout="default" runs land() with its own checkout check, as main() does.
            held = {} if kw.get("checkout") == "default" else {"checkout": kw.get("checkout", same_checkout)}
            if kw.get("clock"):
                held.update(sleep=kw["clock"].sleep, clock=kw["clock"].time)
            else:
                held.update(sleep=lambda s: None)
            try:
                code = land(opts, Git(work), api, sink, scan=kw.get("scan", clean_scan), **held)
            except Refused as e:
                print(str(e), file=sink)
                code = e.code
            except RuntimeError as e:
                # The fake's word for a landing that would never have ended.
                print("NEVER ENDED: %s" % e, file=sink)
                code = "never ended"
            return code, sink.getvalue()

        def remote_ref(bare, ref):
            done = subprocess.run(["git", "--git-dir", bare, "rev-parse", "--verify", "--quiet", ref], stdout=subprocess.PIPE)
            return done.stdout.decode().strip() or None

        # 1. The whole way: signed commits, a green pipeline, the other maintainer's approval.
        bare, work = scenario("happy")
        master_before = remote_ref(bare, "refs/heads/master")
        commit(work, "first, signed")
        tip = commit(work, "second, signed")
        api = FakeGitLab(bare, approved_by=[zain])
        del scans[:]
        del checkouts[:]
        code, text = attempt(work, api)
        expect("happy: exit", code, 0)
        expect("happy: the secret scan read the batch's commits, from master to the tip", scans, [(work, master_before, tip)])
        expect("happy: the checkout's copies were held to master's", checkouts, [(work, master_before)])
        expect("happy: what the scan said is shown", "scan-secrets: nothing found" in text, True)
        expect("happy: the project's master is the tip", remote_ref(bare, "refs/heads/master"), tip)
        expect("happy: the local master too", run(work, "rev-parse", "master"), tip)
        expect("happy: the branch is gone on the project", remote_ref(bare, "refs/heads/batch1"), None)
        created = [c for c in api.calls if c[0] == "POST" and c[1].endswith("/merge_requests")]
        expect("happy: one merge request, not squashed", [c[2].get("squash") for c in created], [False])
        expect("happy: its description names each gate job it waits for, the Windows suite's halves among them",
               [("`semgrep`, `windows-gate`, `windows-gate-s1vm`), and every thread" in (c[2].get("description") or ""))
                for c in created], [True])
        merges = [c for c in api.calls if c[0] == "PUT"]
        expect("happy: merged at exactly the tip", [c[2].get("sha") for c in merges], [tip])
        expect("happy: a landing record naming the approver", len(api.notes) == 1 and "yes: @zainulabidin1990" in api.notes[0], True)
        expect("happy: the local log", os.path.exists(os.path.join(work, ".git", "land-via-mr.log")), True)
        expect("happy: the pipeline's jobs were read to their second page",
               [c[1].split("?")[1] for c in api.calls if "/pipelines/7/jobs" in c[1]], ["per_page=100&page=1", "per_page=100&page=2"])
        expect("happy: the landing record gives each gate job's status", len(api.notes) == 1 and (
            "- Gate jobs: semgrep-sast success, semgrep-sast-gate success, mr-dependency-check success, checkers success, "
            "static-analysis success, semgrep success, windows-gate success, windows-gate-s1vm success\n") in api.notes[0], True)
        expect("happy: the landing record says that the checkout's copies were master's", len(api.notes) == 1 and
               "- The checkout's copies of what the landing runs: master's\n" in api.notes[0], True)
        expect("happy: the landing record says that no check of GitLab's hosted runners was in the pipeline",
               len(api.notes) == 1 and "- GitLab's hosted runners' checks: not in the pipeline (no switch or lane was set)\n"
               in api.notes[0], True)
        expect("happy: the landing record gives the gates' base, where the batch leaves master", len(api.notes) == 1 and
               "\n- The gates' base: `%s`, where the batch leaves master, as %s said\n" % (master_before[:12], " and ".join(
                   BASE_GATES)) in api.notes[0], True)
        expect("happy: each line of the landing record's list is '- what: its value'", [line for line in (
            api.notes[0] if len(api.notes) == 1 else "- none").split("\n") if line.startswith("- ") and
            not re.match(r"^- [^:\n]+: \S", line)], [])

        # 1a. A pipeline a switch was set for: compute-quota, allowed to fail whatever it says,
        # holds no landing, and what it came to is in the landing record and the output.
        bare, work = scenario("hosted")
        commit(work, "signed")
        jobs = full_pipeline() + [{"id": 3000, "name": "hosted-runners", "status": "success", "allow_failure": False},
                                  {"id": 3001, "name": "compute-quota", "status": "failed", "allow_failure": True}]
        api = FakeGitLab(bare, approved_by=[zain], jobs=jobs)
        code, text = attempt(work, api)
        expect("hosted: landed although compute-quota failed", code, 0)
        expect("hosted: the landing record gives what the two checks came to", len(api.notes) == 1 and
               "- GitLab's hosted runners' checks: hosted-runners success, compute-quota failed while allowed to fail\n"
               in api.notes[0], True)
        expect("hosted: and so does the output", "checks of pipeline #7: hosted-runners success, compute-quota failed while "
               "allowed to fail" in text, True)

        # 2. A commit without its sign-off: nothing leaves the machine.
        bare, work = scenario("unsigned")
        commit(work, "signed")
        commit(work, "not signed", signoff=False)
        api = FakeGitLab(bare, approved_by=[zain])
        code, text = attempt(work, api)
        expect("unsigned: exit", code, 4)
        expect("unsigned: nothing pushed", remote_ref(bare, "refs/heads/batch1"), None)
        expect("unsigned: no API call", api.calls, [])
        expect("unsigned: the commit is named", "MISSING" in text and "not signed" in text, True)

        # 2a. A possible secret in the batch, or a scan that could not run: nothing leaves the machine.
        bare, work = scenario("secret")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain])
        finding = "0123456789abcdef0123456789abcdef01234567:a.txt:generic-api-key:2"
        code, text = attempt(work, api, scan=lambda repo, base, tip: (1, finding + "\nscan-secrets: 1 possible secret\n"))
        expect("secret: exit", code, 8)
        expect("secret: nothing pushed", remote_ref(bare, "refs/heads/batch1"), None)
        expect("secret: no API call", api.calls, [])
        expect("secret: the fingerprint is shown", finding in text and "found a possible secret" in text, True)
        code, text = attempt(work, api, scan=lambda repo, base, tip: (3, "scan-secrets: no scanner\n"))
        expect("no scanner: exit", code, 8)
        expect("no scanner: nothing pushed", remote_ref(bare, "refs/heads/batch1"), None)
        expect("no scanner: said so", "had no scanner to run" in text, True)
        code, text = attempt(work, api, scan=lambda repo, base, tip: (2, "scan-secrets: the scan failed\n"))
        expect("a failed scan: exit", code, 8)
        expect("a failed scan: no API call", api.calls, [])

        # 2b. A checkout whose copies of the files a landing runs are not master's - behind
        #     it, edited, or the batch's own worktree - refuses before the sign-off check, the
        #     scan or any API call.
        bare, work = scenario("stale-checkout")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain])
        del scans[:]
        code, text = attempt(work, api, checkout=lambda repo, base: ["build/scan-secrets.py", ".gitlab/ci/gitlab-security.yml"])
        expect("a stale checkout: exit", code, 2)
        expect("a stale checkout: nothing pushed", remote_ref(bare, "refs/heads/batch1"), None)
        expect("a stale checkout: no API call, no scan", (api.calls, scans), ([], []))
        expect("a stale checkout: the files are named, and what to do",
               "build/scan-secrets.py, .gitlab/ci/gitlab-security.yml are not as origin/master" in text
               and "bring that checkout to" in text, True)
        # The sign-off check is one of the copies: loaded - its code run - only once the
        # checkout's copies are known to be the base's, never before a refusal of them.
        held_load_sign_off = globals()["_load_sign_off"]
        loaded = []
        globals()["_load_sign_off"] = lambda: loaded.append("sign-off.py") or held_load_sign_off()
        try:
            code, text = attempt(work, api, checkout=lambda repo, base: ["build/sign-off.py"])
            expect("a checkout whose sign-off.py is not master's: exit", code, 2)
            expect("a checkout whose sign-off.py is not master's: the sign-off check is not loaded", loaded, [])
            code, text = attempt(work, FakeGitLab(bare, approved_by=[zain]), dry_run=True)
            expect("a checkout with master's copies: exit", code, 0)
            expect("a checkout with master's copies: the sign-off check is loaded, once", loaded, ["sign-off.py"])
        finally:
            globals()["_load_sign_off"] = held_load_sign_off
        # checkout_differs itself, over a root that has master's copies, or not.
        bare, work = scenario("checkout-copies")
        held_copies = dict((path, "the base's %s\n" % path) for path in CHECKOUT_COPIES if path != "build/sign-off.py")
        run(work, "checkout", "-q", "master")
        base = commit_files(work, held_copies, "the files a landing runs, signed")
        run(work, "push", "-q", "origin", "master")
        root = os.path.join(tmp, "checkout-copies", "root")

        def root_with(copies):
            shutil.rmtree(root, ignore_errors=True)
            for path, content in copies.items():
                full = os.path.join(root, *path.split("/"))
                if not os.path.isdir(os.path.dirname(full)):
                    os.makedirs(os.path.dirname(full))
                with io.open(full, "w", newline="") as f:
                    f.write(content)
            return checkout_differs(work, base, root=root)

        expect("a root with the base's copies, sign-off.py absent from both", root_with(held_copies), [])
        expect("a root checked out CRLF", root_with(dict((p, c.replace("\n", "\r\n")) for p, c in held_copies.items())), [])
        expect("a root with one copy edited", root_with(dict(held_copies, **{"build/scan-secrets.py": "edited\n"})),
               ["build/scan-secrets.py"])
        expect("a root that lacks one", root_with(dict((p, c) for p, c in held_copies.items() if p != ".gitlab/ci/gitlab-security.yml")),
               [".gitlab/ci/gitlab-security.yml"])
        expect("a root that has one the base lacks", root_with(dict(held_copies, **{"build/sign-off.py": "new\n"})),
               ["build/sign-off.py"])
        expect("a base the repository does not have counts as none", checkout_differs(work, "f" * 40, root=root),
               [p for p in CHECKOUT_COPIES])
        # And in a landing: refused from a root that is not the base's, landed from one that is.
        run(work, "checkout", "-q", "-b", "batch2")
        commit(work, "signed")
        root_with(dict(held_copies, **{"build/ci/secret-detection-rules.sh": "behind\n"}))
        code, text = attempt(work, FakeGitLab(bare, approved_by=[zain]), branch="batch2",
                             checkout=lambda repo, base: checkout_differs(repo, base, root=root))
        expect("a root one file behind: exit", code, 2)
        expect("a root one file behind: named", "build/ci/secret-detection-rules.sh is not as origin/master" in text, True)
        root_with(held_copies)
        code, text = attempt(work, FakeGitLab(bare, approved_by=[zain]), branch="batch2",
                             checkout=lambda repo, base: checkout_differs(repo, base, root=root))
        expect("a root with the base's copies: exit", code, 0)
        # A batch that repairs what master's copies cannot land: --own-copies lands it from
        # its own worktree, every copy its tip's, the reason and the files kept in the
        # landing record and the log; it is refused without a reason, with a blank one,
        # from a root with a copy that is neither master's nor the batch's, and from one
        # with master's copy of a file the batch changes beside the batch's of another.
        repaired = {"build/scan-secrets.py": "the batch's scan-secrets.py\n",
                    "build/ci/secret-detection-rules.sh": "the batch's rules script\n"}
        run(work, "checkout", "-q", "-b", "batch3", "master")
        tip3 = commit_files(work, repaired, "the scan repaired, signed")
        own_root = dict(held_copies, **repaired)
        why = "the owner: master's secret_detection image can no longer be pulled, 13 October 2026"
        read = "read whole by the owner, 13 October 2026"
        on_root = lambda repo, rev: checkout_differs(repo, rev, root=root)
        for what, copies, given, said in (
                ("the batch's own copies without --own-copies", own_root, None, "with --own-copies \"<who, why>\""),
                ("the batch's own copies with a blank reason", own_root, "   ", "with --own-copies \"<who, why>\""),
                ("--own-copies from a root with a copy neither master's nor the batch's",
                 dict(own_root, **{"build/land-via-mr.py": "edited\n"}), why, "whose build/land-via-mr.py is not as batch3"),
                ("--own-copies from a root with master's copy of a file the batch changes",
                 dict(own_root, **{"build/ci/secret-detection-rules.sh": held_copies["build/ci/secret-detection-rules.sh"]}),
                 why, "whose build/ci/secret-detection-rules.sh is not as batch3")):
            root_with(copies)
            api = FakeGitLab(bare, approved_by=[zain])
            del scans[:]
            code, text = attempt(work, api, branch="batch3", own_copies=given, gates_read=read, checkout=on_root)
            expect("%s: exit" % what, code, 2)
            expect("%s: nothing pushed, no API call, no scan" % what,
                   (remote_ref(bare, "refs/heads/batch3"), api.calls, scans), (None, [], []))
            expect("%s: said so" % what, said in text, True)
        root_with(own_root)
        api = FakeGitLab(bare, approved_by=[zain])
        code, text = attempt(work, api, branch="batch3", own_copies=why, gates_read=read, checkout=on_root)
        expect("the batch's own copies on --own-copies: exit", code, 0)
        expect("the batch's own copies on --own-copies: master is the tip", remote_ref(bare, "refs/heads/master"), tip3)
        expect("the batch's own copies on --own-copies: warned, with the reason",
               "WARNING: this landing runs from the batch's own copies" in text and why in text, True)
        expect("the batch's own copies on --own-copies: the landing record keeps the files and the reason",
               len(api.notes) == 1 and ("- The checkout's copies of what the landing runs: the batch's own, not master's "
                                        "(build/scan-secrets.py, build/ci/secret-detection-rules.sh), on --own-copies: %s"
                                        % why) in api.notes[0], True)
        with io.open(os.path.join(work, ".git", "land-via-mr.log"), encoding="utf-8") as f:
            logged = f.read().splitlines()
        expect("the batch's own copies on --own-copies: so does the log", bool(logged) and "\tbatch3\t" in logged[-1] and
               ("the checkout's copies: the batch's own, not master's (build/scan-secrets.py, "
                "build/ci/secret-detection-rules.sh), on --own-copies: %s" % why) in logged[-1], True)
        # --own-copies from master's copies changes nothing, and says so.
        run(work, "checkout", "-q", "-b", "batch4", "master")
        commit(work, "signed")
        root_with(own_root)
        code, text = attempt(work, FakeGitLab(bare, approved_by=[zain]), branch="batch4", own_copies=why, dry_run=True,
                             checkout=on_root)
        expect("--own-copies from master's copies: exit", code, 0)
        expect("--own-copies from master's copies: not needed, and said so",
               "--own-copies is not needed" in text and "WARNING" not in text, True)
        # What the landing runs from its checkout: this script and the sign-off check, and
        # everything build/scan-secrets.py takes from its own tree.
        scanner = importlib.util.spec_from_file_location("scan_secrets_own", os.path.join(ROOT, "build", "scan-secrets.py"))
        scanner_module = importlib.util.module_from_spec(scanner)
        scanner.loader.exec_module(scanner_module)
        expect("CHECKOUT_COPIES hold this script, sign-off.py and scan-secrets.py's OWN",
               sorted((set(scanner_module.OWN) | {"build/land-via-mr.py", "build/sign-off.py"}) - set(CHECKOUT_COPIES)), [])
        # What a real landing runs: main() calls land() with no scan and no checkout check
        # of its own, so land()'s defaults - build/scan-secrets.py, and this checkout's
        # copies held to the base - are what holds every landing. A default replaced, or a
        # main() that passed a stand-in, would turn either off with every gate green and
        # every case above, which pass their own, still passing.
        parameters = inspect.signature(land).parameters
        expect("land()'s secret scan is secret_scan unless a caller gives another",
               parameters["scan"].default is secret_scan, True)
        expect("land()'s checkout check is checkout_differs unless a caller gives another",
               parameters["checkout"].default is checkout_differs, True)
        held_land = globals()["land"]
        landed = []
        globals()["land"] = lambda *a, **kw: landed.append((a, kw)) or 0
        try:
            main(["batch1", "--dry-run"])
        finally:
            globals()["land"] = held_land
        expect("main() runs land() with land()'s own secret scan and checkout check",
               [(len(a), sorted(k for k in kw if k in ("scan", "checkout"))) for a, kw in landed], [(4, [])])
        # ... and land() run so, from this tree, over a project whose master has none of
        # the files it runs: refused before the scan and any API call, each file named.
        bare, work = scenario("default-checkout")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain])
        del scans[:]
        code, text = attempt(work, api, checkout="default")
        expect("land()'s own checkout check, this tree against another master: exit", code, 2)
        expect("land()'s own checkout check: no API call, no scan", (api.calls, scans), ([], []))
        expect("land()'s own checkout check: each of the files named", [p for p in CHECKOUT_COPIES if p not in text], [])

        # 2c. A worktree with master checked out keeps it, and is named, to be brought to
        #     the tip before the next landing.
        bare, work = scenario("master-checked-out")
        master_before = remote_ref(bare, "refs/heads/master")
        tip = commit(work, "signed")
        held_master = os.path.join(tmp, "master-checked-out", "held-master-worktree")
        run(work, "worktree", "add", "-q", held_master, "master")
        code, text = attempt(work, FakeGitLab(bare, approved_by=[zain]))
        expect("master checked out: exit", code, 0)
        expect("master checked out: the local master is left", run(work, "rev-parse", "master"), master_before)
        expect("master checked out: the worktree is named, and what to do",
               "A worktree has master checked out (" in text and os.path.basename(held_master) in text
               and "merge --ff-only %s" % tip in text and "the next landing" in text, True)

        # 3. Behind master: refused before the push.
        bare, work = scenario("behind")
        commit(work, "signed")
        other = os.path.join(tmp, "behind", "other")
        subprocess.run(["git", "clone", "-q", bare, other], check=True)
        commit(other, "master moved")
        run(other, "push", "-q", "origin", "HEAD:master")
        api = FakeGitLab(bare)
        code, text = attempt(work, api)
        expect("behind: exit", code, 3)
        expect("behind: nothing pushed", remote_ref(bare, "refs/heads/batch1"), None)

        # 4. A merge commit in the batch.
        bare, work = scenario("merge")
        commit(work, "signed")
        run(work, "checkout", "-q", "-b", "side", "master")
        with io.open(os.path.join(work, "b.txt"), "w") as f:
            f.write("side\n")
        run(work, "add", "b.txt")
        run(work, "commit", "-q", "-s", "-m", "side, signed")
        run(work, "checkout", "-q", "batch1")
        run(work, "merge", "-q", "--no-ff", "--signoff", "-m", "a merge", "side")
        code, text = attempt(work, FakeGitLab(bare))
        expect("merge commit: exit", code, 3)

        # 5. A failed pipeline: no merge.
        bare, work = scenario("failed")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain], pipeline_statuses=["running", "failed"])
        code, text = attempt(work, api)
        expect("failed pipeline: exit", code, 5)
        expect("failed pipeline: no merge call", [c for c in api.calls if c[0] == "PUT"], [])
        expect("failed pipeline: master unmoved", remote_ref(bare, "refs/heads/master") != remote_ref(bare, "refs/heads/batch1"), True)

        # 6. A pipeline blocked on a manual job is not a pass.
        bare, work = scenario("manual")
        commit(work, "signed")
        code, text = attempt(work, FakeGitLab(bare, pipeline_statuses=["manual"]))
        expect("manual pipeline: exit", code, 5)

        # 6a. GitLab moves a merge request's head to a pushed commit a moment after the push
        #     (batch47's landing, 14 October 2026): a head still the commit before is read
        #     again until it is the tip, for up to HEAD_WAIT_SECONDS from the push or from
        #     finding the branch there, and refused only then, its pipeline never taken for
        #     the tip's; a head that leaves the tip once it was there refuses at once.
        def opened_at_first_tip(name):
            """A merge request opened (--no-wait) at the branch's first tip, and a commit on top:
            (bare, work, api, the first tip, the commit on top)."""
            bare, work = scenario(name)
            first = commit(work, "signed, the merge request's first head")
            api = FakeGitLab(bare, approved_by=[zain])
            code, _ = attempt(work, api, no_wait=True)
            expect("%s: the merge request opened at the first tip" % name, (code, len(api.mrs)), (0, 1))
            run(work, "checkout", "-q", "batch1")
            return bare, work, api, first, commit(work, "signed, pushed on top")

        bare, work, api, first, tip = opened_at_first_tip("head-late")
        # Two reads with the commit before as the head, its pipeline failed; one with the head
        # moved and the head pipeline still the commit before's; then the branch as it is.
        api.heads = [(first, first, "failed"), (first, first, "failed"), (None, first, "failed")]
        clock = Clock()
        code, text = attempt(work, api, clock=clock)
        expect("a head that reaches the tip late: landed", code, 0)
        expect("a head that reaches the tip late: master is the tip", remote_ref(bare, "refs/heads/master"), tip)
        expect("a head that reaches the tip late: merged at the tip", [c[2].get("sha") for c in api.calls if c[0] == "PUT"],
               [tip])
        expect("a head that reaches the tip late: said what it waited for",
               ("Merge request !1's head is %s (its head pipeline's commit: %s), not yet %s: GitLab moves it a moment after "
                "the push to origin/batch1. Waiting for it to be %s, up to 5 minute(s) from then, reading it every %d s."
                % (first[:12], first[:12], tip[:12], tip[:12], HEAD_POLL_SECONDS)) in text, True)
        expect("a head that reaches the tip late: and when it came",
               ("Merge request !1's head is %s, %d s after the push to origin/batch1; its head pipeline's commit: %s (the "
                "pipeline of %s is looked for by its commit)." % (tip[:12], 2 * HEAD_POLL_SECONDS, first[:12], tip[:12]))
               in text, True)
        expect("a head that reaches the tip late: two reads waited for", [s for s in clock.slept if s],
               [HEAD_POLL_SECONDS, HEAD_POLL_SECONDS])
        expect("a head that reaches the tip late: the commit before's failed pipeline not taken for the tip's",
               ("Pipeline #6" not in text, "Pipeline of %s: not yet created" % tip[:12] in text,
                "Pipeline #7 https://example.invalid/p/7: success" in text), (True, True, True))
        bare, work, api, first, tip = opened_at_first_tip("head-late-found")
        code, _ = attempt(work, api, no_wait=True)
        expect("head-late-found: an earlier run pushed the tip", (code, remote_ref(bare, "refs/heads/batch1")), (0, tip))
        # The commit before as the head once, its pipeline a success: what must not be landed on.
        api.heads = [(first, first, "success")]
        clock = Clock()
        code, text = attempt(work, api, clock=clock)
        expect("a head late after an earlier run's push: landed", (code, remote_ref(bare, "refs/heads/master")), (0, tip))
        expect("a head late after an earlier run's push: said from when it counted",
               ("Merge request !1's head is %s, %d s after this run found origin/batch1 at it; its head pipeline's commit: "
                "%s." % (tip[:12], HEAD_POLL_SECONDS, tip[:12])) in text and "Pipeline #6" not in text, True)
        bare, work, api, first, tip = opened_at_first_tip("head-stuck")
        # The commit before stays the head, its pipeline a success; the search for the open
        # merge request after the push takes a minute, which the five minutes count in.
        api.heads = [(first, first, "success")] * 100
        clock = Clock()
        api.clock, api.lookup_seconds = clock, 60
        code, text = attempt(work, api, clock=clock)
        expect("a head that never reaches the tip: refused", code, 5)
        expect("a head that never reaches the tip: no merge call, master unmoved",
               ([c for c in api.calls if c[0] == "PUT"], remote_ref(bare, "refs/heads/master") == tip), ([], False))
        expect("a head that never reaches the tip: refused %d s after the push, a minute of it the search" % HEAD_WAIT_SECONDS,
               (sum(clock.slept), clock.now - (1760000000.0 + 60)), (HEAD_WAIT_SECONDS - 60, HEAD_WAIT_SECONDS - 60))
        expect("a head that never reaches the tip: said so, and what to do",
               ("REFUSED: merge request !1's head is still %s (its head pipeline's commit: %s), not the %s checked here, 5 "
                "minute(s) after the push to origin/batch1: GitLab has not moved the merge request to it; run this again"
                % (first[:12], first[:12], tip[:12])) in text, True)
        expect("a head that never reaches the tip: no pipeline looked at", "Pipeline " in text, False)
        bare, work = scenario("head-moved-again")
        tip = commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain])
        other = "c" * 40
        api.heads = [(None, None, None), (other, other, None)]
        clock = Clock()
        code, text = attempt(work, api, clock=clock)
        expect("a head that leaves the tip once it was there: refused", code, 5)
        expect("a head that leaves the tip once it was there: at once, and said so", (
            [s for s in clock.slept if s], "REFUSED: merge request !1's head is %s, not the %s checked here; run this again"
            % (other[:12], tip[:12]) in text, "Waiting" in text), ([], True, False))
        expect("a head that leaves the tip once it was there: no merge call", [c for c in api.calls if c[0] == "PUT"], [])
        expect("the head is waited for five minutes, read every ten seconds", (HEAD_WAIT_SECONDS, HEAD_POLL_SECONDS), (300, 10))

        # 7. Threads open: GitLab says no, and so does this.
        bare, work = scenario("threads")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain], mergeable="discussions_not_resolved")
        code, text = attempt(work, api)
        expect("open threads: exit", code, 6)
        expect("open threads: no merge call", [c for c in api.calls if c[0] == "PUT"], [])

        # 8. No second person: refused; the owner's override lands it and says so; an
        #    override from anybody else, or with no reason, is refused.
        bare, work = scenario("alone")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[chris_old])
        code, text = attempt(work, api)
        expect("alone: exit", code, 7)
        expect("alone: no merge call", [c for c in api.calls if c[0] == "PUT"], [])
        expect("alone: master unmoved", remote_ref(bare, "refs/heads/master") != remote_ref(bare, "refs/heads/batch1"), True)
        expect("alone: recorded as no, not merged", len(api.notes) == 1
               and "NO. Approved only by the author's own account(s): @chrisholloway5" in api.notes[0]
               and "not merged" in api.notes[0], True)
        expect("alone: the refusal names the override", "--land-without-approval" in text, True)
        bare, work = scenario("required")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[])
        code, text = attempt(work, api)
        expect("nobody: exit", code, 7)
        expect("nobody: no merge call", [c for c in api.calls if c[0] == "PUT"], [])
        expect("nobody: recorded as not merged", len(api.notes) == 1 and "not merged" in api.notes[0], True)
        bare, work = scenario("override")
        tip = commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[])
        code, text = attempt(work, api, override="security fix inside its disclosure window")
        expect("owner's override: exit", code, 0)
        expect("owner's override: master is the tip", remote_ref(bare, "refs/heads/master"), tip)
        expect("owner's override: warned", "WARNING: no second person approved" in text, True)
        expect("owner's override: recorded with its reason", len(api.notes) == 1 and "WITHOUT it" in api.notes[0]
               and "security fix inside its disclosure window" in api.notes[0], True)
        bare, work = scenario("override-other")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[], lander="zainulabidin1990")
        code, text = attempt(work, api, override="in a hurry")
        expect("another's override: exit", code, 7)
        expect("another's override: no merge call", [c for c in api.calls if c[0] == "PUT"], [])
        expect("another's override: recorded as refused", len(api.notes) == 1 and "only the owner may override" in api.notes[0], True)
        bare, work = scenario("override-blank")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[])
        code, text = attempt(work, api, override="   ")
        expect("a blank reason is no override: exit", code, 7)
        expect("a blank reason is no override: no merge call", [c for c in api.calls if c[0] == "PUT"], [])

        # 9. Approvals read from the system notes where the approvals API is refused.
        bare, work = scenario("notes")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain], approvals_api=False)
        code, text = attempt(work, api)
        expect("notes: exit", code, 0)
        expect("notes: the approval was found there", len(api.notes) == 1 and "yes: @zainulabidin1990" in api.notes[0]
               and "@joker" not in api.notes[0] and "system notes" in api.notes[0], True)

        # 9a. An approval given before the tip reached the merge request is not one of the tip.
        bare, work = scenario("stale")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain], approved_at="2026-09-26T09:00:00.000Z")
        code, text = attempt(work, api)
        expect("stale approval: exit", code, 7)
        expect("stale approval: no merge call", [c for c in api.calls if c[0] == "PUT"], [])
        expect("stale approval: recorded as given before the tip", len(api.notes) == 1 and "@zainulabidin1990 before" in api.notes[0], True)
        bare, work = scenario("stale-notes")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain], approved_at="2026-09-26T09:59:59.999Z", approvals_api=False)
        code, text = attempt(work, api)
        expect("stale approval in the notes: refused", code == 7 and [c for c in api.calls if c[0] == "PUT"] == [], True)
        bare, work = scenario("no-versions")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain], approved_at="", versions_api=False)
        code, text = attempt(work, api)
        expect("versions unreadable: counted, and said so", code == 0 and len(api.notes) == 1 and "yes: @zainulabidin1990" in api.notes[0]
               and "could not be read" in api.notes[0], True)
        expect("timestamps parse and order", when("2026-09-26T10:00:00.000Z") < when("2026-09-26T10:00:01Z"), True)
        expect("an unknown time is none", when(""), None)
        expect("an approval of unknown time is not the tip's, once the tip's arrival is known",
               current_approvals([{"username": "z", "at": ""}], when("2026-09-26T10:00:00Z")), ([], [{"username": "z", "at": ""}]))

        # 10. A protected branch GitLab keeps after the merge: landed, and said so.
        bare, work = scenario("kept")
        commit(work, "signed")
        code, text = attempt(work, FakeGitLab(bare, approved_by=[zain], protected_branch=True))
        expect("kept branch: exit", code, 0)
        expect("kept branch: said so", "is still on GitLab" in text, True)

        # 11. Commits a release tag holds are not asked for a sign-off; the rest are.
        bare, work = scenario("tagged")
        old = commit(work, "tagged, not signed off", signoff=False)
        run(work, "tag", "v1.0.1", old)
        run(work, "push", "-q", "origin", "v1.0.1")
        commit(work, "after the tag, signed")
        code, text = attempt(work, FakeGitLab(bare, approved_by=[zain]))
        expect("tagged: exit", code, 0)
        bare, work = scenario("tag-local-only")
        old = commit(work, "tagged here only, not signed off", signoff=False)
        run(work, "tag", "v9.9.9", old)
        code, text = attempt(work, FakeGitLab(bare))
        expect("a tag only this clone has exempts nothing", code, 4)

        # 12. Not a batch branch; and a dry run changes nothing.
        bare, work = scenario("names")
        commit(work, "signed")
        run(work, "branch", "feature", "batch1")
        code, text = attempt(work, FakeGitLab(bare), branch="feature")
        expect("not batch*: exit", code, 2)
        api = FakeGitLab(bare)
        code, text = attempt(work, api, dry_run=True)
        expect("dry run: exit", code, 0)
        expect("dry run: nothing pushed", remote_ref(bare, "refs/heads/batch1"), None)
        expect("dry run: no API call", api.calls, [])
        code, text = attempt(work, api, dry_run=True, scan=lambda repo, base, tip: (1, "scan-secrets: 1 possible secret\n"))
        expect("a dry run scans too", code, 8)

        # 13. A pipeline GitLab calls a success, without a gate that held: a source scan a
        #     rule trimmed from it, and a change gate that failed while allowed to fail.
        bare, work = scenario("trimmed")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain], jobs=full_pipeline(**{"static-analysis": None}))
        code, text = attempt(work, api)
        expect("trimmed pipeline: exit", code, 5)
        expect("trimmed pipeline: no merge call", [c for c in api.calls if c[0] == "PUT"], [])
        expect("trimmed pipeline: the missing scan is named", "static-analysis not in the pipeline" in text
               and "but static-analysis did not" in text, True)
        bare, work = scenario("allowed-to-fail")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain], jobs=full_pipeline(**{"semgrep-sast-gate": ("failed", True)}))
        code, text = attempt(work, api)
        expect("a gate that failed while allowed to fail: exit", code, 5)
        expect("a gate that failed while allowed to fail: no merge call", [c for c in api.calls if c[0] == "PUT"], [])
        expect("a gate that failed while allowed to fail: named, and said so",
               "semgrep-sast-gate failed while allowed to fail" in text, True)
        # checkers, which holds the CI files to the gates: a CI file that let it fail
        # would let a pipeline pass with the gates' jobs changed unseen.
        bare, work = scenario("checkers-allowed-to-fail")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain], jobs=full_pipeline(checkers=("failed", True)))
        code, text = attempt(work, api)
        expect("checkers failed while allowed to fail: exit", code, 5)
        expect("checkers failed while allowed to fail: named", "checkers failed while allowed to fail" in text, True)
        for name in GATE_JOBS:
            bare, work = scenario("missing-" + name)
            commit(work, "signed")
            code, text = attempt(work, FakeGitLab(bare, approved_by=[zain], jobs=full_pipeline(**{name: None})))
            expect("%s missing: exit" % name, code, 5)
        bare, work = scenario("skipped-scan")
        commit(work, "signed")
        code, text = attempt(work, FakeGitLab(bare, approved_by=[zain], jobs=full_pipeline(semgrep=("skipped", False))))
        expect("a skipped scan: exit", code, 5)
        # The Windows suite's two halves: one that a busy bench ended with exit 10, which
        # the pipeline lets fail, and one the pipeline lacks, each hold the batch.
        for name in WINDOWS_GATE_JOBS:
            bare, work = scenario("busy-" + name)
            commit(work, "signed")
            api = FakeGitLab(bare, approved_by=[zain], jobs=full_pipeline(**{name: ("failed", True)}))
            code, text = attempt(work, api)
            expect("%s let pass by a busy bench: exit" % name, code, 5)
            expect("%s let pass by a busy bench: no merge call" % name, [c for c in api.calls if c[0] == "PUT"], [])
            expect("%s let pass by a busy bench: named, with the way to gate it" % name,
                   "%s failed while allowed to fail" % name in text and "is gated by retrying its job" in text, True)
            bare, work = scenario("missing-" + name)
            commit(work, "signed")
            code, text = attempt(work, FakeGitLab(bare, approved_by=[zain], jobs=full_pipeline(**{name: None})))
            expect("%s missing: exit" % name, code, 5)
            expect("%s missing: named" % name, "%s not in the pipeline" % name in text, True)
        expect("the Windows suite's halves are the jobs a landing needs of it", WINDOWS_GATE_JOBS,
               ("windows-gate", "windows-gate-s1vm"))

        # 14. A batch of documents alone - Markdown at the root and under a dot-directory -
        #     needs the change gates and not the scans or the Windows suite's halves,
        #     which .own-runners creates none of; one anywhere else is not documents alone.
        bare, work = scenario("documents")
        tip = commit_files(work, {"README.md": "read me\n", ".gitlab/notes.md": "notes\n"}, "documents, signed")
        api = FakeGitLab(bare, approved_by=[zain], jobs=full_pipeline(**{"static-analysis": None, "semgrep": None,
                                                                         "windows-gate": None, "windows-gate-s1vm": None}))
        code, text = attempt(work, api)
        expect("documents alone: exit", code, 0)
        expect("documents alone: master is the tip", remote_ref(bare, "refs/heads/master"), tip)
        expect("documents alone: the record says why the scans and the Windows halves were not needed", len(api.notes) == 1
               and "static-analysis, semgrep, windows-gate and windows-gate-s1vm not required" in api.notes[0], True)
        bare, work = scenario("documents-no-gate")
        commit_files(work, {"README.md": "read me\n"}, "documents, signed")
        api = FakeGitLab(bare, approved_by=[zain], jobs=full_pipeline(**{"static-analysis": None, "semgrep": None,
                                                                         "mr-dependency-check": None}))
        code, text = attempt(work, api)
        expect("documents alone still need the change gates: exit", code, 5)
        bare, work = scenario("documents-elsewhere")
        commit_files(work, {"hmailserver/docs/Notes.md": "notes\n"}, "a document under a directory, signed")
        api = FakeGitLab(bare, approved_by=[zain], jobs=full_pipeline(**{"static-analysis": None, "semgrep": None}))
        code, text = attempt(work, api)
        expect("Markdown under a directory without a dot is not documents alone: exit", code, 5)
        expect("documents_only reads paths as .own-runners does",
               [documents_only(p) for p in (["README.md"], [".gitlab/a/b.md"], ["docs/a.md"], ["README.md", "a.txt"],
                                            ["CHANGELOG"], [])], [True, True, False, False, False, False])

        # 15. A batch that changes the gates' own files needs a person's word that the
        #     change was read; nothing is pushed without it, and it is recorded.
        bare, work = scenario("gate-files")
        tip = commit_files(work, {"build/ci/security-gate.py": "print('a gate')\n"}, "a gate changed, signed")
        api = FakeGitLab(bare, approved_by=[zain])
        code, text = attempt(work, api)
        expect("the gates' files changed, unread: exit", code, 9)
        expect("the gates' files changed, unread: nothing pushed", remote_ref(bare, "refs/heads/batch1"), None)
        expect("the gates' files changed, unread: the file is named", "build/ci/security-gate.py" in text, True)
        code, text = attempt(work, api, gates_read="read whole by the owner, 6 October 2026")
        expect("the gates' files changed, read: exit", code, 0)
        expect("the gates' files changed, read: master is the tip", remote_ref(bare, "refs/heads/master"), tip)
        expect("the gates' files changed, read: recorded", len(api.notes) == 1 and
               "changed (build/ci/security-gate.py), read: read whole by the owner" in api.notes[0], True)

        # 16. A pipeline another project ran - a fork's, on runners its owner chose - is
        #     not the gate.
        bare, work = scenario("fork-pipeline")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain], pipeline_project=999)
        code, text = attempt(work, api)
        expect("a pipeline another project ran: exit", code, 5)
        expect("a pipeline another project ran: said so", "ran in project 999" in text, True)
        expect("a pipeline another project ran: no merge call", [c for c in api.calls if c[0] == "PUT"], [])

        # 17. Each gate that finds its base must have judged the batch against where it
        #     leaves master: a log that names another base, or none (the base's copy's
        #     line is not the gate's own), refuses.
        for what, said, needle in (("another base", "f" * 40, "judged it against ffffffffffff"),
                                   ("no base", "", "says no base")):
            bare, work = scenario("gate-base-" + what.replace(" ", "-"))
            commit(work, "signed")
            api = FakeGitLab(bare, approved_by=[zain], gate_base=said)
            code, text = attempt(work, api)
            expect("a gate's log with %s: exit" % what, code, 5)
            expect("a gate's log with %s: said so" % what, needle in text, True)
            expect("a gate's log with %s: no merge call" % what, [c for c in api.calls if c[0] == "PUT"], [])
        bare, work = scenario("gate-base")
        commit(work, "signed")
        api = FakeGitLab(bare, approved_by=[zain])
        code, text = attempt(work, api)
        expect("the gates' logs name master's tip: exit", code, 0)
        expect("the gates' logs were read, the latest attempt of each",
               sorted(c[1] for c in api.calls if c[1].endswith("/trace")), ["projects/1/jobs/2001/trace", "projects/1/jobs/2002/trace"])

        # 18. A batch that changes the gates' code lands only when each gate's log says
        #     that master's copy of its script judged it and passed it, or that master has
        #     none: a line the base's copy printed, or one that says it failed, is not that.
        for what, said, code_wanted in (("no word of the base's copy", None, 5),
                                        ("the base's copy's own line alone", "  base's copy: " + BASE_COPY_RAN, 5),
                                        ("the base's copy failed", "The base's copy of %s exited 1.", 5),
                                        ("the base's copy failed, ten", "The base's copy of %s exited 10.", 5),
                                        ("the base's copy passed, its line timed",
                                         "2026-10-07T04:00:00.123456Z 00O " + BASE_COPY_RAN, 0),
                                        ("no copy at the base", BASE_COPY_NONE, 0),
                                        ("the base's copy passed", BASE_COPY_RAN, 0)):
            bare, work = scenario("base-copy-" + what.replace(" ", "-").replace("'", ""))
            tip = commit_files(work, {"build/ci/mr-dependency-check.py": "print('a gate')\n"}, "a gate's code, signed")
            api = FakeGitLab(bare, approved_by=[zain], base_copy=said)
            code, text = attempt(work, api, gates_read="read whole by the owner, 7 October 2026")
            expect("the gates' code changed, %s: exit" % what, code, code_wanted)
            if code_wanted:
                expect("the gates' code changed, %s: said so" % what,
                       "does not say that master's copy of build/ci/mr-dependency-check.py and build/ci/security-gate.py "
                       "judged it" in text, True)
                expect("the gates' code changed, %s: no merge call" % what, [c for c in api.calls if c[0] == "PUT"], [])
            else:
                expect("the gates' code changed, %s: recorded" % what, len(api.notes) == 1 and
                       "Master's copy of the gates' code: judged the batch and passed it" in api.notes[0], True)
        # A batch that changes a gate's job, not its code, needs no word of the base's copy.
        bare, work = scenario("base-copy-not-needed")
        commit_files(work, {".gitlab/ci/no-github.yml": "# a change\n"}, "a gate's job file, signed")
        api = FakeGitLab(bare, approved_by=[zain], base_copy=None)
        code, text = attempt(work, api)
        expect("a gate's job file changed, unread: exit", code, 9)
        code, text = attempt(work, api, gates_read="read whole by the owner, 7 October 2026")
        expect("a gate's job file changed, read, no base's copy: exit", code, 0)
        expect("a gate's job file changed: the record says the base's copy was not needed", len(api.notes) == 1 and
               "Master's copy of the gates' code: not needed" in api.notes[0], True)
        for path in (".gitlab-ci.yml", ".gitlab/ci/no-github.yml", "build/sign-off.py", "build/scan-secrets.py",
                     "build/ci/static-analysis.sh", "build/ci/semgrep.sh"):
            expect("%s is one of the gates' own files" % path, path in GATE_FILES, True)
        expect("checkers is a job a landing needs", "checkers" in GATE_JOBS, True)
        # A source scan's script changed outside its marks (how the job ends) needs a
        # person's word.
        bare, work = scenario("scan-script")
        commit_files(work, {"build/ci/semgrep.sh": "#!/bin/bash\nexit 0\n"}, "a scan's script, signed")
        code, text = attempt(work, FakeGitLab(bare, approved_by=[zain]))
        expect("a scan's script changed, unread: exit", code, 9)
        expect("a scan's script changed, unread: named", "build/ci/semgrep.sh" in text, True)
        expect("the gates' code is in the gates' own files", set(GATE_CODE) <= set(GATE_FILES), True)
    finally:
        for key, value in env_keep.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        shutil.rmtree(tmp, ignore_errors=True)
    for f in failures:
        print("FAIL " + f)
    print("land-via-mr.py self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    parser = argparse.ArgumentParser(description="Land a batch* branch on master through a GitLab merge request.")
    parser.add_argument("branch")
    parser.add_argument("--remote")
    parser.add_argument("--project", default=os.environ.get("HM_GITLAB_PROJECT", PROJECT))
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--no-wait", action="store_true")
    parser.add_argument("--force-push", action="store_true")
    parser.add_argument("--timeout-hours", type=float, default=12)
    parser.add_argument("--poll-seconds", type=int, default=60)
    parser.add_argument("--land-without-approval", metavar="REASON",
                        help="the owner's override of the approval rule for this one landing; REASON is recorded")
    parser.add_argument("--gates-read", metavar="REASON",
                        help="the batch changes the gates' own files, and a person has read that change whole; REASON is "
                             "recorded")
    parser.add_argument("--own-copies", metavar="REASON",
                        help="this checkout is the batch's own worktree, whose copies of the files a landing runs judge it, "
                             "since master's cannot land it and the batch repairs them; REASON says who and why, and is "
                             "recorded")
    opts = parser.parse_args(argv)
    repo = subprocess.run(["git", "rev-parse", "--show-toplevel"], stdout=subprocess.PIPE).stdout.decode().strip() or os.getcwd()
    try:
        return land(opts, Git(repo), Glab(os.environ.get("HM_GITLAB_HOST", HOST)), sys.stdout)
    except Refused as e:
        print(str(e))
        return e.code
    except ApiError as e:
        print("GitLab: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
