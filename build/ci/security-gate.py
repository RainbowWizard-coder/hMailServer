#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Before a release is signed: the security scans of its commit read, and every finding addressed.

The release job (build/ci/release-chain.py collect) calls evaluate() before anything is
signed, with the artifacts of the four scans the job needs; the hand runbook runs this
script on the same reports made on linix. The policy is SECURITY.md's "Vulnerability
management policy", "Before a release":

  static-analysis   cppcheck and clang-tidy (build/ci/static-analysis.sh): every finding
                    not in build/ci/static-analysis-baseline.txt;
  semgrep           Semgrep's open rules (build/ci/semgrep.sh): every finding not in
                    build/ci/semgrep-baseline.txt;
  semgrep-sast      GitLab's Semgrep analyzer with GitLab's rules
                    (.gitlab/ci/gitlab-security.yml): every finding of severity
                    Critical or High;
  dependency-scan   Trivy and OSV (build/ci/dependency-scan.py): every vulnerability
                    of severity Critical or High with a fixed version that the VEX
                    document (.gitlab/hmailserver.openvex.json) does not dismiss;

each blocks the release unless .gitlab/security-exceptions.yml - master's, in the
release job, as check_tag in release-chain.py fetched it - accepts it: an entry
naming the scan and the finding, with the reason it is accepted, who reviewed it, when,
and an expiry at most a year after the review that has not passed. And a scan whose
report is missing, or says the scan failed, blocks it whatever it found: a scan that did
not finish has said nothing is clean. So does dependency-scan when it did not scan the
CI image, whose packages the Linux packages are built against, a semgrep-sast report
GitLab's analyzer did not write or that began before the tag's pipeline was created,
and a tagged tree that carries a scan's report as a tracked file (committed_reports),
which would stand in for one the scan did not write.

A finding is named for an exception as the gate prints it: the source scans by the
20-character fingerprint build/ci/security-findings.py gives a finding (the tool, the
rule, the file and the text of the flagged line - not its number), semgrep-sast by the
same fingerprint made from GitLab's report, and dependency-scan by the advisory's id and
the package.

The exceptions file is a small, strict subset of YAML, read here without a YAML library
(the release job's image has none): comments, "exceptions:" (or "exceptions: []"), and a
list of flat entries of "key: value" lines. Anything else is refused with its line number.

Usage, from the root of the checkout the scans read:
  python3 build/ci/security-gate.py [--artifacts DIR] [--exceptions FILE] [--today YYYY-MM-DD]
  python3 build/ci/security-gate.py --merge-request BASE|auto [--artifacts DIR] [--exceptions FILE]
  python3 build/ci/security-gate.py --self-test
--artifacts is the directory holding the scans' artifacts as the jobs write them
(security/static-analysis/, security/semgrep/, security/dependency-scan/ and
gl-sast-report.json); the checkout's root by default. Exits 0 when the release may go
on, 1 when it may not, 2 on a usage error.

--merge-request is a change's gate instead (the semgrep-sast-gate job,
.gitlab/ci/mr-checks.yml): semgrep-sast's report alone, the exceptions as the commit
BASE has them, and the findings in the files the change touches from BASE - or in
every file, when it alters the scan's configuration, where, when it re-pins the
analyzer, its own copy of the exceptions may accept a finding in a file it does not
touch (merge_request_evaluate below); and, whatever the scan found, no entry added to
a baseline or reason taken from one, no suppression comment added to a file a scan
reads, no narrowing of what a scan reads but one that lands alone, no source file put
out of the scan's reach, and no changed file the scan gave up on (change_problems,
sarif_holes; a SARIF this cannot read as SARIF_READ_FOR's analyzer writes it is an
input that cannot be read). The report must be the analyzer's, begun after the
pipeline was created (CI_PIPELINE_CREATED_AT), and no commit may carry a scan's report
(committed_reports). When the code that runs is not the base's gates' code, the base's
copy judges the change too, whatever any variable says (base_copy_verdict).
--exceptions, with it, stands for the base's file. Exits 0 when
the change may land, 1 when it may not, 2 when its inputs cannot be read. BASE "auto"
is change_base's: CI_MERGE_REQUEST_DIFF_BASE_SHA in a merge request's pipeline, when
it is an ancestor of HEAD and of the target branch, and in a batch* branch's pipeline,
which GitLab holds the branch's merge request to, where HEAD leaves master, fetched by
name; a base that cannot be found exits 2, and a CI file of the change that defines a
variable GitLab gives (CI_*, GITLAB_*) exits 1.

These hold against mistakes, not against a hostile change: the change's own CI files
say which jobs run and what they run (SECURITY.md, "What the gates cannot hold").
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
import fnmatch
import importlib.util
import json
import re
import shutil
import subprocess
import tempfile
import uuid
import xml.etree.ElementTree as ET

ROOT =os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXCEPTIONS = ".gitlab/security-exceptions.yml"

# The four scans, where their reports are, and what blocks. The release job needs each
# with its artifacts (.gitlab-ci.yml); build/check-release-chain.py holds the two to
# each other.
SCANS = ["static-analysis", "semgrep", "semgrep-sast", "dependency-scan"]
SOURCE_REPORTS = {
    "static-analysis": [os.path.join("security", "static-analysis", "gl-sast-cppcheck.json"),
                        os.path.join("security", "static-analysis", "gl-sast-clang-tidy.json")],
    "semgrep": [os.path.join("security", "semgrep", "gl-sast-semgrep.json")],
}
BASELINES = {
    "static-analysis": os.path.join("build", "ci", "static-analysis-baseline.txt"),
    "semgrep": os.path.join("build", "ci", "semgrep-baseline.txt"),
}
GITLAB_SAST_REPORT = "gl-sast-report.json"
# What GitLab's analyzer leaves beside its report: Semgrep's own SARIF, whose
# notifications name each file a rule gave up on or the parser could not read.
SEMGREP_SARIF = "semgrep.sarif"
# What a scan writes and no commit may carry: a committed copy would stand in for a
# report the scan did not write (committed_reports).
SCAN_OUTPUTS = (GITLAB_SAST_REPORT, SEMGREP_SARIF, "security")
# A clock a shared runner keeps may run behind GitLab's: a report begun this long
# before its pipeline was created is still taken as the pipeline's own.
CLOCK_SKEW_SECONDS = 300
DEPENDENCY_REPORTS = [os.path.join("security", "dependency-scan", "gl-dependency-scanning-trivy.json"),
                      os.path.join("security", "dependency-scan", "gl-dependency-scanning-osv.json")]
DEPENDENCY_JUNIT = os.path.join("security", "dependency-scan", "junit.xml")
BLOCKING_SEVERITIES = ("Critical", "High")
# An exception is reviewed again at least once a year.
LONGEST_EXCEPTION_DAYS = 366
FIELDS = ["scan", "finding", "package", "what", "reason", "reviewed-by", "reviewed", "expires"]
REQUIRED = ["scan", "finding", "what", "reason", "reviewed-by", "reviewed", "expires"]
FINGERPRINT = re.compile(r"^[0-9a-f]{20}$")
ADVISORY = re.compile(r"^[A-Z][A-Z0-9]*-[A-Za-z0-9][A-Za-z0-9._:\-]*$")
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def _load_findings():
    path = os.path.join(ROOT, "build", "ci", "security-findings.py")
    spec = importlib.util.spec_from_file_location("security_findings", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


findings_module = _load_findings()


# ---------------------------------------------------------------- the exceptions file

def _scalar(text, number):
    """A YAML plain, single- or double-quoted scalar on one line."""
    text = text.strip()
    if not text:
        raise ValueError("line %d: a key with no value" % number)
    if text[0] == "'":
        if len(text) < 2 or text[-1] != "'":
            raise ValueError("line %d: a single-quoted value that does not end on its line" % number)
        return text[1:-1].replace("''", "'")
    if text[0] == '"':
        if len(text) < 2 or text[-1] != '"' or text[-2] == "\\":
            raise ValueError("line %d: a double-quoted value that does not end on its line" % number)
        body = text[1:-1]
        if "\\" in body:
            raise ValueError("line %d: escapes in a double-quoted value are not read here; use single quotes" % number)
        return body
    if text[0] in "[]{}&*!|>%@`#":
        raise ValueError("line %d: %r is YAML this file does not use; quote the value" % (number, text[:20]))
    if " #" in text:
        raise ValueError("line %d: a comment after a value; put it on a line of its own" % number)
    return text


def parse_exceptions(text):
    """The entries of the exceptions file, each a dict of strings. ValueError, with the
    line, for anything outside the subset this file is written in."""
    entries = []
    seen_key = False
    current = None
    for number, raw in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
        if "\t" in raw:
            raise ValueError("line %d: a tab; YAML indents with spaces" % number)
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if not seen_key:
            if stripped == "exceptions: []":
                seen_key = "empty"
                continue
            if raw != "exceptions:":
                raise ValueError("line %d: the file begins with 'exceptions:', at the start of the line" % number)
            seen_key = True
            continue
        if seen_key == "empty":
            raise ValueError("line %d: 'exceptions: []' says there are none, and something follows" % number)
        match = re.match(r"^  - ([a-z][a-z\-]*):(?: (.*))?$", raw)
        if match:
            current = {}
            entries.append((number, current))
        else:
            match = re.match(r"^    ([a-z][a-z\-]*):(?: (.*))?$", raw)
            if not match or current is None:
                raise ValueError("line %d: not an entry's '  - key: value' or its '    key: value'" % number)
        key, value = match.group(1), match.group(2)
        if key not in FIELDS:
            raise ValueError("line %d: %r is not a field of an exception (%s)" % (number, key, ", ".join(FIELDS)))
        if key in current:
            raise ValueError("line %d: %s given twice in one entry" % (number, key))
        current[key] = _scalar(value or "", number)
    if not seen_key:
        raise ValueError("the file has no 'exceptions:' key")
    return entries


def _date(value):
    if not DATE.match(value or ""):
        return None
    try:
        return datetime.date(int(value[:4]), int(value[5:7]), int(value[8:10]))
    except ValueError:
        return None


def check_exceptions(text):
    """(entries, problems): every entry that is well formed, and what is wrong with the
    others. Whether an entry has expired is not a problem here - the gate leaves an
    expired one out, and the finding it named blocks again."""
    try:
        numbered = parse_exceptions(text)
    except ValueError as e:
        return [], ["%s: %s" % (EXCEPTIONS, e)]
    entries, problems, keys = [], [], set()
    for number, entry in numbered:
        where = "%s, the entry at line %d" % (EXCEPTIONS, number)
        wrong = []
        missing = [f for f in REQUIRED if not (entry.get(f) or "").strip()]
        if missing:
            wrong.append("it has no %s" % ", ".join(missing))
        scan = entry.get("scan", "")
        if scan and scan not in SCANS:
            wrong.append("its scan is %r, not one of %s" % (scan, ", ".join(SCANS)))
        finding = entry.get("finding", "")
        if scan == "dependency-scan":
            if finding and not ADVISORY.match(finding):
                wrong.append("its finding %r is not an advisory id (CVE-..., GHSA-..., OSV-...)" % finding)
            if not (entry.get("package") or "").strip():
                wrong.append("a dependency-scan exception names the package the advisory is against")
        elif scan:
            if finding and not FINGERPRINT.match(finding):
                wrong.append("its finding %r is not the 20-character fingerprint the gate prints" % finding)
            if "package" in entry:
                wrong.append("only a dependency-scan exception names a package")
        if len((entry.get("reason") or "").strip()) < 30:
            wrong.append("its reason says too little to be read by the next reviewer (at least 30 characters: why "
                         "the finding is not exploitable here, or why it is accepted until the expiry)")
        reviewed, expires = _date(entry.get("reviewed")), _date(entry.get("expires"))
        if entry.get("reviewed") and not reviewed:
            wrong.append("reviewed is not a date (YYYY-MM-DD)")
        if entry.get("expires") and not expires:
            wrong.append("expires is not a date (YYYY-MM-DD)")
        if reviewed and expires:
            if expires <= reviewed:
                wrong.append("it expires on or before the day it was reviewed")
            elif (expires - reviewed).days > LONGEST_EXCEPTION_DAYS:
                wrong.append("it runs %d days from its review; an exception is reviewed again within a year" % (expires - reviewed).days)
        key = (scan, finding, entry.get("package", ""))
        if key in keys:
            wrong.append("another entry names the same finding")
        keys.add(key)
        if wrong:
            problems.append("%s: %s" % (where, "; ".join(wrong)))
        else:
            entries.append(entry)
    return entries, problems


# ---------------------------------------------------------------- the reports

def _read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f), None
    except OSError:
        return None, "there is no %s" % path.replace(os.sep, "/")
    except ValueError as e:
        return None, "%s is not JSON: %s" % (path.replace(os.sep, "/"), e)


def _status_problem(report, relative):
    scan = report.get("scan") if isinstance(report, dict) else None
    if not isinstance(scan, dict):
        return "%s has no scan section: it is not a report the scan finished" % relative
    if scan.get("status") != "success":
        return "%s says the scan's status is %r, not success: a scan that did not finish has said nothing is clean" % (
            relative, scan.get("status"))
    if not isinstance(report.get("vulnerabilities"), list):
        return "%s has no list of vulnerabilities" % relative
    return None


def _timestamp(text):
    """A report's or GitLab's time (2026-10-06T20:43:57, with Z, an offset or a fraction
    or not) as a naive UTC datetime, or None."""
    match = re.match(r"^(\d{4}-\d{2}-\d{2})[T ](\d{2}:\d{2}:\d{2})(?:\.\d+)?(Z|[+-]\d{2}:?\d{2})?$", (text or "").strip())
    if not match:
        return None
    try:
        moment = datetime.datetime.strptime("%s %s" % (match.group(1), match.group(2)), "%Y-%m-%d %H:%M:%S")
    except ValueError:
        return None
    zone = match.group(3)
    if zone and zone != "Z":
        sign = 1 if zone[0] == "+" else -1
        digits = zone[1:].replace(":", "")
        moment -= sign * datetime.timedelta(hours=int(digits[:2]), minutes=int(digits[2:]))
    return moment


def pipeline_created(environ=None):
    """When this pipeline was created (CI_PIPELINE_CREATED_AT), or None outside one."""
    environ = os.environ if environ is None else environ
    return _timestamp(environ.get("CI_PIPELINE_CREATED_AT") or "")


def provenance_problem(report, not_before):
    """Why semgrep-sast's report is not the one this pipeline's analyzer wrote, or None:
    GitLab's Semgrep analyzer wrote it (scan.analyzer.id), and, given not_before (when
    the pipeline was created), it began then or after. A report committed to the tree, or
    left by an analyzer run by hand, would otherwise stand in for one the job never
    wrote."""
    scan = report.get("scan") or {}
    analyzer = (scan.get("analyzer") or {}).get("id") if isinstance(scan.get("analyzer"), dict) else None
    if analyzer != "semgrep":
        return ("%s was not written by GitLab's Semgrep analyzer (its scan.analyzer.id is %r, not 'semgrep'): it is not "
                "semgrep-sast's report" % (GITLAB_SAST_REPORT, analyzer))
    if not_before is not None:
        started = _timestamp(scan.get("start_time") or "")
        if started is None:
            return "%s gives no start_time this can read (%r): it cannot be told for this pipeline's" % (
                GITLAB_SAST_REPORT, scan.get("start_time"))
        if started < not_before - datetime.timedelta(seconds=CLOCK_SKEW_SECONDS):
            return ("%s says its scan began at %s, before this pipeline was created at %s: it is an older report than "
                    "this pipeline's semgrep-sast wrote" % (GITLAB_SAST_REPORT, started.isoformat(), not_before.isoformat()))
    return None


def committed_reports(root, run=None):
    """The problems of a tree that carries a scan's output (SCAN_OUTPUTS) as a tracked
    file: a committed gl-sast-report.json, semgrep.sarif or security/ would stand in
    for what the scan did not write. A tree git cannot list is a problem too."""
    run = run or subprocess.run
    listed = run(["git", "-C", root, "ls-files", "-z", "--"] + list(SCAN_OUTPUTS), capture_output=True)
    if listed.returncode != 0:
        return ["git ls-files could not list %s: whether the tree carries a scan's report cannot be told (%s)" % (
            root, (listed.stderr or listed.stdout or b"")[:200])]
    out = listed.stdout if isinstance(listed.stdout, str) else (listed.stdout or b"").decode("utf-8", "replace")
    tracked = sorted(p for p in out.split("\0") if p)
    if not tracked:
        return []
    return ["the tree carries what a scan writes, as tracked files (%s%s): a committed report stands in for one the "
            "scan did not write, so none may be committed; git rm --cached them, and .gitignore keeps them out" % (
                ", ".join(tracked[:5]), " and %d more" % (len(tracked) - 5) if len(tracked) > 5 else "")]


def _baseline_ids(root, scan):
    """The report ids of the baseline's findings: security-findings.py makes a finding's
    id from its fingerprint, the same way for every run."""
    known = findings_module.read_baseline(os.path.join(root, BASELINES[scan]))
    return dict((str(uuid.uuid5(findings_module.ID_NAMESPACE, fp)), fp) for fp in known)


def _fingerprint_of(vulnerability):
    for identifier in vulnerability.get("identifiers") or []:
        if isinstance(identifier, dict) and identifier.get("type") == "hmailserver_fingerprint":
            return identifier.get("value") or ""
    return ""


def _place(vulnerability):
    location = vulnerability.get("location") or {}
    line = location.get("start_line")
    return "%s%s" % (location.get("file") or "unknown", (":%s" % line) if line else "")


def source_scan(root, artifacts, scan, accepted):
    """static-analysis or semgrep: (blocking, accepted, notes)."""
    problems, blocking, taken = [], [], []
    known = _baseline_ids(root, scan)
    excepted = dict((str(uuid.uuid5(findings_module.ID_NAMESPACE, e["finding"])), e) for e in accepted)
    total = 0
    for relative in SOURCE_REPORTS[scan]:
        report, error = _read_json(os.path.join(artifacts, relative))
        if error:
            problems.append("%s: %s - the job did not finish its scan, or its artifacts did not reach this job" % (scan, error))
            continue
        status = _status_problem(report, relative.replace(os.sep, "/"))
        if status:
            problems.append("%s: %s" % (scan, status))
            continue
        for v in report["vulnerabilities"]:
            total += 1
            vid = v.get("id") or ""
            if vid in known:
                continue
            fingerprint = _fingerprint_of(v)
            if vid in excepted:
                taken.append((excepted[vid], v))
                continue
            blocking.append("%s: a finding not in %s: %s %s at %s [%s]%s" % (
                scan, BASELINES[scan].replace(os.sep, "/"), v.get("severity", "Unknown"), (v.get("description") or "")[:160],
                _place(v), v.get("name", ""),
                (" - fingerprint %s" % fingerprint) if fingerprint else
                " - its fingerprint is in the job's findings (build/ci/security-findings.py)"))
    return problems + blocking, taken, total


def gitlab_sast_scan(root, artifacts, accepted, scope=None, outside=None, own=None, own_taken=None, not_before=None):
    """semgrep-sast, GitLab's report: every Critical or High finding, fingerprinted as
    security-findings.py fingerprints its own (the rule, the file, the line's text).
    scope, a set of paths, holds only the findings in those files (a merge request's
    changed files); the others are listed in outside, when given, and hold nothing.
    own, (the changed paths, exceptions), is a change's own copy of the exceptions in
    force, which accepts a finding only in a file the change does not touch (when the
    change re-pins the analyzer, merge_request_evaluate); what it accepts goes to
    own_taken. A report GitLab's analyzer did not write, or, given not_before, one begun
    before the pipeline was created, is no report (provenance_problem)."""
    relative = GITLAB_SAST_REPORT
    report, error = _read_json(os.path.join(artifacts, relative))
    if error:
        return (["semgrep-sast: %s - the job did not run for this commit, failed before its report, or its artifact "
                 "did not reach this job (.gitlab/ci/gitlab-security.yml keeps it)" % error], [], 0)
    status = _status_problem(report, relative) or provenance_problem(report, not_before)
    if status:
        return ["semgrep-sast: %s" % status], [], 0
    found = []
    for v in report["vulnerabilities"]:
        location = v.get("location") or {}
        rule = ""
        for identifier in v.get("identifiers") or []:
            if isinstance(identifier, dict) and identifier.get("type") == "semgrep_id":
                rule = identifier.get("value") or ""
                break
        rule = rule or v.get("name") or v.get("id") or "unknown"
        f = findings_module.finding("semgrep-sast", rule, location.get("file") or "", location.get("start_line") or 0, 0,
                                    v.get("severity") or "Unknown", v.get("description") or v.get("name") or "")
        f["report"] = v
        found.append(f)
    findings_module.fingerprint_all(found)
    excepted = dict((e["finding"], e) for e in accepted)
    own_paths, own_entries = own if own is not None else (frozenset(), [])
    own_excepted = dict((e["finding"], e) for e in own_entries)
    blocking, taken = [], []
    for f in found:
        if f["severity"] not in BLOCKING_SEVERITIES:
            continue
        if f["fingerprint"] in excepted:
            taken.append((excepted[f["fingerprint"]], f["report"]))
            continue
        if f["fingerprint"] in own_excepted and f["path"] not in own_paths:
            if own_taken is not None:
                own_taken.append((own_excepted[f["fingerprint"]], f["report"]))
            continue
        described = "semgrep-sast: %s %s at %s:%d [%s] - fingerprint %s" % (
            f["severity"], f["message"][:160], f["path"], f["line"], f["rule"], f["fingerprint"])
        if f["fingerprint"] in own_excepted:
            described += (" - accepted only by this change's own %s, which accepts nothing in a file the change touches: "
                          "the exception lands first, by a change of its own" % EXCEPTIONS)
        if scope is not None and f["path"] not in scope:
            if outside is not None:
                outside.append(described)
            continue
        blocking.append(described)
    return blocking, taken, len(found)


def dependency_scan(root, artifacts, accepted):
    problems, blocking, taken = [], [], []
    excepted = dict(((e["finding"], e["package"]), e) for e in accepted)
    total = 0
    for relative in DEPENDENCY_REPORTS:
        report, error = _read_json(os.path.join(artifacts, relative))
        if error:
            problems.append("dependency-scan: %s - the job did not finish, or its artifacts did not reach this job" % error)
            continue
        status = _status_problem(report, relative.replace(os.sep, "/"))
        if status:
            problems.append("dependency-scan: %s" % status)
            continue
        for v in report["vulnerabilities"]:
            total += 1
            if v.get("severity") not in BLOCKING_SEVERITIES or not v.get("solution"):
                continue
            advisory = ""
            for identifier in v.get("identifiers") or []:
                if isinstance(identifier, dict) and identifier.get("value"):
                    advisory = identifier["value"]
                    break
            package = (((v.get("location") or {}).get("dependency") or {}).get("package") or {}).get("name") or ""
            if (advisory, package) in excepted:
                taken.append((excepted[(advisory, package)], v))
                continue
            blocking.append("dependency-scan: %s %s in %s (%s): %s - fixed upstream (%s), and neither the VEX document "
                            "nor an exception dismisses it" % (
                                v.get("severity"), advisory or "an advisory", package or "a package",
                                (v.get("location") or {}).get("file", "unknown"), (v.get("name") or "")[:120],
                                (v.get("solution") or "")[:120]))
    # The CI image: the Linux packages are built against its libraries, and a scan
    # that could not read them has not said they are clean.
    junit_path = os.path.join(artifacts, DEPENDENCY_JUNIT)
    try:
        suite = ET.parse(junit_path).getroot()  # nosemgrep: our own job's output, read in our own job
    except (OSError, ET.ParseError) as e:
        problems.append("dependency-scan: %s cannot be read (%s)" % (DEPENDENCY_JUNIT.replace(os.sep, "/"), e))
        suite = None
    if suite is not None:
        image = [case for case in suite.iter("testcase") if case.get("classname") == "dependency-scan.image"]
        if not image or any(case.find("skipped") is not None for case in image):
            problems.append("dependency-scan did not scan the CI image's packages (ci-image-packages' artifact was not "
                            "there), whose OpenSSL, libpq, libldap and Boost the Linux packages are built against")
    return problems + blocking, taken, total


def _in_force(entries, today, say):
    """({scan: [the exceptions in force today]}, problems): an exception reviewed after
    today is a problem; one past its expiry is said and left out, so the finding it
    named holds again."""
    in_force, lapsed, problems = {}, [], []
    for e in entries:
        if _date(e["reviewed"]) > today:
            problems.append("%s: the exception for %s %s says it was reviewed on %s, which is after today, %s" % (
                EXCEPTIONS, e["scan"], e["finding"], e["reviewed"], today.isoformat()))
        elif _date(e["expires"]) < today:
            lapsed.append(e)
        else:
            in_force.setdefault(e["scan"], []).append(e)
    for e in lapsed:
        say("expired on %s, so no longer accepted: %s %s (%s)" % (e["expires"], e["scan"], e["finding"], e["what"]))
    return in_force, problems


def evaluate(root, artifacts, today=None, exceptions_text=None, say=print, not_before=None):
    """The problems that hold the release. Nothing when every scan finished and every
    finding the policy counts is in a baseline or accepted by an exception in force.
    not_before, when the pipeline was created, holds a semgrep-sast report begun before
    it (provenance_problem)."""
    today = today or datetime.datetime.now(datetime.timezone.utc).date()
    if exceptions_text is None:
        try:
            with open(os.path.join(root, EXCEPTIONS), encoding="utf-8") as f:
                exceptions_text = f.read()
        except OSError:
            return ["%s is missing: the gate needs the file even when it accepts nothing (exceptions: [])" % EXCEPTIONS]
    entries, problems = check_exceptions(exceptions_text)
    if problems:
        return problems
    in_force, problems = _in_force(entries, today, say)

    results = {}
    for scan in SCANS:
        accepted = in_force.get(scan, [])
        if scan in SOURCE_REPORTS:
            results[scan] = source_scan(root, artifacts, scan, accepted)
        elif scan == "semgrep-sast":
            results[scan] = gitlab_sast_scan(root, artifacts, accepted, not_before=not_before)
        else:
            results[scan] = dependency_scan(root, artifacts, accepted)
    for scan in SCANS:
        found, taken, total = results[scan]
        say("%-16s %5d finding(s) in the report; %d accepted by an exception; %d holding the release" % (
            scan, total, len(taken), len(found)))
        for entry, v in taken:
            say("                 accepted until %s (%s): %s - %s" % (entry["expires"], entry["reviewed-by"], entry["finding"], entry["what"]))
        problems.extend(found)
    return problems


# ---------------------------------------------------------------- a change's gate
#
# In every change's pipeline - a merge request's own, or a protected batch* branch's,
# which GitLab holds that branch's merge request to - semgrep-sast runs, and the
# semgrep-sast-gate job of .gitlab/ci/mr-checks.yml asks this about its report before
# the change can land (OpenSSF Baseline OSPS-VM-06.02: a change with a weakness above
# the threshold is blocked unless it is declared not exploitable). The same rule as the
# release's, Critical and High findings that .gitlab/security-exceptions.yml does not
# accept, with three differences:
#
#   the exceptions are the change's base's, the target branch's copy where the change
#   left it, never the change's own: a change cannot accept its own finding. An
#   exception lands first, by a change of its own - which, with only the exceptions
#   file changed, holds nothing here - and the change after it;
#
#   only the findings in the files the change touches hold it, so a change is
#   answerable for what it touches, not for a finding already on the target branch in
#   a file it never opened: that one holds the next release instead, whose gate reads
#   the whole tree. Unless the change alters the scan itself (SAST_CONFIG: the
#   analyzer, its pin or its options, GitLab's custom ruleset file, or how a finding is
#   fingerprinted), when every finding in the tree is its business. That only widens
#   what counts: a finding the change keeps out of the report is never seen, which the
#   third difference is for. When the change re-pins semgrep-sast's analyzer, and only
#   then, a finding in a file the change does not touch may also be accepted by the
#   change's own copy of the exceptions file, and one in a file it touches still by the
#   base's alone: a change that re-pins a vanished analyzer while the target branch
#   holds findings nobody has accepted can carry their exceptions with it, which no
#   change of its own could land first, since the scan that would judge that change is
#   the one gone;
#
#   a change that narrows what a scan reads (scope_narrowings: a .semgrepignore,
#   GitLab's custom ruleset, an exclusion that reaches a path the base's did not, the
#   analyzer's options, the ignore and suppression lists of the scans on linix - a line
#   gained, or a "!" line lost or put before what it takes back - clang-tidy's checks, or
#   the lines of static-analysis.sh and semgrep.sh that choose what their tools read and
#   report) is a declaration, as an exception is: it lands alone, by a change that
#   touches nothing else a scan reads, and the change after it is judged as it says.
#
# And whatever the scan found, a change may not take the ways of silencing a finding
# without a declaration on the base (change_problems): it may not add an entry to a
# source scan's baseline or take an entry's reason away, raise, in any file a scan
# reads, the count of the comments by which a tool skips a finding, move a source file
# out of semgrep-sast's reach - git's move, or a file deleted and one of its name added
# where the scan reads nothing - or add one where GitLab's analyzer reads nothing and no
# exclusion of the project's says so (reach_problems), or change a file the scan read
# only in part (sarif_holes).

# The files whose change can change what semgrep-sast finds anywhere, or what this gate
# makes of it: a change that alters one answers for every finding in the tree.
# .gitlab-ci.yml among them, since a variable it sets for every job (SAST_EXCLUDED_PATHS,
# SAST_EXCLUDED_ANALYZERS, SAST_RULESET_GIT_REFERENCE...) reaches the scan as
# gitlab-security.yml's own do; and mr-checks.yml, which runs this gate.
#
# .gitlab/sast-ruleset.toml is the file GitLab's analyzers read a custom ruleset from
# (rules disabled, overridden or added): none is committed, and the one that is added
# changes the scan as much as an edit of its job does.
SAST_CONFIG = (".gitlab-ci.yml", ".gitlab/ci/gitlab-security.yml", ".gitlab/ci/mr-checks.yml", ".gitlab/sast-ruleset.toml",
               "build/ci/security-gate.py", "build/ci/security-findings.py")
SAST_JOB_FILE = ".gitlab/ci/gitlab-security.yml"

# The CI files: .gitlab-ci.yml and every YAML file under .gitlab/, which it includes.
CI_FILE = re.compile(r"^\.gitlab-ci\.yml$|^\.gitlab/.+\.ya?ml$")
# The files that list what a scan leaves out, a line each: a line one gains narrows the
# scan. Semgrep's .semgrepignore at any depth (GitLab's analyzer renames the root's to
# .semgrepignore.local and includes it, and Semgrep reads one in a directory too), and
# the two scans on linix's: build/ci/semgrepignore and cppcheck's suppressions list.
SCOPE_LISTS = re.compile(r"(?:^|/)\.semgrepignore[^/]*$|^build/ci/semgrepignore$|^build/ci/cppcheck-suppressions\.txt$")
# The ones in .gitignore's syntax, Semgrep's, where a line that begins with "!" takes
# back what a line before it left out: a "!" line lost, or put before the line whose
# path it takes back, narrows the scan as a line gained does. cppcheck's list has none.
IGNORE_SYNTAX = re.compile(r"(?:^|/)\.semgrepignore[^/]*$|^build/ci/semgrepignore$")
# The scripts of the two scans on linix, whose own lines choose what cppcheck,
# clang-tidy and Semgrep read and report: the files given them, the options that leave
# a file, a check or a finding out, the rule packs. Those lines stand between a line
# SCAN_SCOPE_MARK begins and one it ends, and a change of one of them, gained or lost or
# moved - an option taken away can narrow as well as one added (--include, --enable) - is
# a narrowing; and so is one of the tools' options (SCAN_OPTION) a change puts outside
# them, or marks that do not pair (scan_scope).
SCAN_SCRIPTS = ("build/ci/static-analysis.sh", "build/ci/semgrep.sh")
SCAN_SCOPE_MARK = re.compile(r"^[ \t]*# scan-scope (begin|end)\b")
SCAN_OPTION = re.compile(r"(?<![\w-])(?:-i|--suppress|--suppressions-list|--inline-suppr|--enable|--disable|--file-filter|"
                         r"--project|--library|--checks|--config-file|--config|--header-filter|--exclude-header-filter|"
                         r"--line-filter|--include|--exclude|--exclude-rule|--severity|--baseline-commit|--max-target-bytes|"
                         r"--timeout|--timeout-threshold|--oss-only|--pro)(?=[=\s\"']|$)")
# And the files any change to which can narrow one: GitLab's custom ruleset, which
# disables or overrides rules, and the checks clang-tidy runs (static-analysis.sh gives
# it this file with --config-file, so no .clang-tidy of the tree is read).
SCOPE_FILES = re.compile(r"^\.gitlab/sast-ruleset\.toml$|^build/ci/clang-tidy\.yaml$")
# The variables that choose what semgrep-sast reads and reports, defined in any CI file:
# GitLab's analyzer reads SAST_*, SEMGREP_* and SECURE_* ones, and static-analysis.sh
# HM_TIDY_CHECKS. The two lists of what the analyzer leaves out are compared entry by
# entry (EXCLUSION_VARIABLES); a change of any other is a change of scope.
# A key of a mapping: at a line's start after its indentation (and a sequence's dash),
# or after a flow mapping's brace or comma - not a word in a script's string.
SCOPE_VARIABLE = re.compile(r"""(?:^[ \t]*(?:-[ \t]+)?|[{,][ \t]*)["']?((?:SAST|SEMGREP|SECURE)_[A-Z0-9_]+|HM_TIDY_CHECKS)["']?[ \t]*:(?=[ \t]|$)""",
                            re.MULTILINE)
EXCLUSION_VARIABLES = ("SAST_EXCLUDED_PATHS", "SAST_SEMGREP_EXCLUDED_PATHS")
# GitLab's analyzer's own ignore template (/semgrepignore in its image, which it copies
# to the checkout's .semgrepignore before Semgrep runs; analyzer 6.26.1), in .gitignore's
# syntax: a name with no slash, or with a trailing one alone, is a file or a directory
# at any depth. It ends by including the project's own .semgrepignore.
ANALYZER_IGNORES = (".gitignore", ".git/", "node_modules/", "build/", "dist/", "vendor/", ".env/", ".venv/", ".tox/",
                    "*.min.js", ".semgrep", ".semgrep_logs/")
# Where the template leaves code unread and the project says so (SECURITY.md, "What no
# gate reads"): the build and CI scripts under build/ at the root, which the semgrep job
# on linix reads in a batch.
DECLARED_UNREAD = ("build/",)
# The files semgrep-sast reads: the extensions of the languages GitLab's rules are for.
SAST_SOURCE = re.compile(r"\.(?:c|cc|cpp|cxx|h|hh|hpp|hxx|inl|ipp|cs|py|pyw|js|jsx|mjs|cjs|ts|tsx|java|go|rb|php|scala|"
                         r"kt|kts|swift|rs|sh|bash)$", re.IGNORECASE)
# The kinds of Semgrep notification that say a rule gave up on a file, which is then
# read by some rules or none: a time limit or the memory or stack it ran out of. And
# the kinds that say the parser stumbled: Semgrep's parsers recover from most of what
# they cannot read and scan the rest - over batch44 on 6 October 2026, 87 of the C++
# and C# files drew one ("_T" strings run together, WINAPI, a class over two lines),
# and 18 of them still had findings - and the notification does not say how much was
# left out, so those are counted, not held.
SARIF_GAVE_UP = ("Timeout", "Out of memory", "Stack overflow", "Fixpoint timeout", "Timeout during interfile analysis",
                 "OOM during interfile analysis")
SARIF_PARSE = ("Syntax error", "Lexical error", "Other syntax error", "AST builder error", "PartialParsing")
# Both lists, and the file a notification names in its text, are Semgrep's wording,
# which a new analyzer can change: a notification of a kind in neither, or of a kind
# that gave up on a file whose text names no file, cannot be read (exit 2), never
# passed over. The wording was read from analyzer 6.26.1 (Semgrep 1.174.0) on 6 and 7
# October 2026, a rule's timeout over a file made to take long and the parser's
# notifications over batch44's tree, and SARIF_FIXTURE is what it wrote, word for word:
# the self-test reads it, and fails while semgrep-sast's job pins another analyzer than
# SARIF_READ_FOR, until the wording is read again (build/ci/mirror-semgrep.sh's header).
SARIF_READ_FOR = "6.26.1"
SARIF_FIXTURE = (
    {"descriptor": {"id": "Timeout"}, "level": "warning",
     "message": {"text": "Timeout when running tmp.slow-pattern on src/deep/big.py:\n "}},
    {"descriptor": {"id": "Syntax error"}, "level": "warning",
     "message": {"text": "Syntax error at line hmailserver/source/Server/COM/COMAuthentication.cpp:130:\n `WINAPI` was "
                         "unexpected"}},
    {"descriptor": {"id": "Other syntax error"}, "level": "warning",
     "message": {"text": "Other syntax error at line hmailserver/source/Server/Common/Util/RestApiSettings.cpp:1:\n "
                         "Failure: ii_of_name: IdDeref"}})
# What a notification's text names as a file: a path with an extension, standing alone.
SARIF_PATH = re.compile(r"(?<![\w./-])[\w.+-]+(?:/[\w.+-]+)*\.[A-Za-z0-9]+(?=[:\s'\"]|$)")


def _uncommented(text):
    """The text with YAML's comments taken out: from a # at a line's start or after a
    blank, outside quotes."""
    lines = []
    for line in text.replace("\r\n", "\n").split("\n"):
        quote, cut = None, len(line)
        for i, ch in enumerate(line):
            if quote:
                if ch == quote:
                    quote = None
            elif ch in "\"'":
                quote = ch
            elif ch == "#" and (i == 0 or line[i - 1] in " \t"):
                cut = i
                break
        lines.append(line[:cut])
    return "\n".join(lines)


def scope_variables(texts):
    """{name: [each value a CI file gives it]} for the variables of SCOPE_VARIABLE that
    the CI files texts ({path: text}) define, in a block mapping or a flow one. A value
    this cannot read on its line - a block scalar, an anchor's alias, a tag - is None."""
    found = {}
    for path in sorted(texts):
        for line in _uncommented(texts[path] or "").split("\n"):
            for match in SCOPE_VARIABLE.finditer(line):
                rest = line[match.end():].strip()
                flow = "{" in line[:match.start(1)]
                if not rest or rest[0] in "|>*&!":
                    value = None
                elif rest[0] in "\"'":
                    end = rest.find(rest[0], 1)
                    while rest[0] == "'" and end > 0 and rest[end + 1:end + 2] == "'":
                        end = rest.find("'", end + 2)
                    value = rest[1:end].replace("''", "'") if end > 0 else None
                else:
                    value = re.split(r"[,}]", rest)[0].strip() if flow else rest
                found.setdefault(match.group(1), []).append(value)
    return found


def _pattern_parts(pattern):
    """(anchored, directory only, the parts) of an exclusion, read as Semgrep's --exclude
    reads it: anchored at the root when it begins with a slash or has one before its
    last character."""
    text = pattern.strip()
    parts = [p for p in text.strip("/").split("/") if p]
    return text.startswith("/") or len(parts) > 1, text.endswith("/"), parts


def excluded_by(pattern, path):
    """Whether the exclusion pattern leaves path out, as Semgrep's --exclude reads it
    (.gitignore's syntax) or as GitLab's pathfilter reads a report's paths: a pattern
    with no slash names a file or a directory at any depth - "tests" leaves out
    a/b/tests/c.py - and one with a slash is held to the root. Either is enough."""
    anchored, directory, parts = _pattern_parts(pattern)
    names = path.split("/")
    if not parts:
        return False
    if not anchored:
        return any(fnmatch.fnmatchcase(name, parts[0]) for name in (names[:-1] if directory else names))
    if any("**" in p for p in parts):
        return fnmatch.fnmatchcase(path, "/".join(parts).replace("**", "*")) or \
            fnmatch.fnmatchcase(path, "/".join(parts).replace("**", "*") + "/*")
    if len(names) < len(parts) or (directory and len(names) == len(parts)):
        return False
    return all(fnmatch.fnmatchcase(name, part) for name, part in zip(names, parts))


def subsumed(entry, by):
    """Whether every path the exclusion entry leaves out, the exclusion by leaves out too:
    so that an entry a change adds narrows nothing - /tests under tests, a path under a
    directory already left out. An entry with a wildcard is subsumed only by itself."""
    if entry.strip() == by.strip():
        return True
    anchored, directory, parts = _pattern_parts(entry)
    by_anchored, by_directory, by_parts = _pattern_parts(by)
    if not parts or not by_parts or any(c in "".join(parts) for c in "*?[") or any("**" in p for p in by_parts):
        return False
    if not anchored:
        # A name at any depth: only a name at any depth, the same or a wider glob, holds it.
        return not by_anchored and fnmatch.fnmatchcase(parts[0], by_parts[0]) and (directory or not by_directory)
    if not by_anchored:
        # Every path under an anchored entry has the entry's names among its own; a
        # directory-only name must be one of them before the last.
        named = parts if directory or not by_directory else parts[:-1]
        return any(fnmatch.fnmatchcase(name, by_parts[0]) for name in named)
    if len(parts) < len(by_parts) or (by_directory and len(parts) == len(by_parts) and not directory):
        return False
    return all(fnmatch.fnmatchcase(name, part) for name, part in zip(parts, by_parts))


def exclusions(variables):
    """(the entries of the two exclusion lists, readable): every entry of every value the
    CI files give SAST_EXCLUDED_PATHS and SAST_SEMGREP_EXCLUDED_PATHS, which the
    analyzer passes to Semgrep alike; readable is False when one is a value this cannot
    read."""
    entries, readable = [], True
    for name in EXCLUSION_VARIABLES:
        for value in variables.get(name, []):
            if value is None:
                readable = False
                continue
            entries.extend(e.strip() for e in value.split(",") if e.strip())
    return entries, readable


def unread_reason(path, entries):
    """None when semgrep-sast reads path; else ("listed", the entry) when an exclusion of
    the project's leaves it out, or ("template", the pattern) when only the analyzer's
    own template does."""
    for entry in entries:
        if excluded_by(entry, path):
            return ("listed", entry)
    for pattern in ANALYZER_IGNORES:
        if excluded_by(pattern, path):
            return ("template", pattern)
    return None


def lines_gained(base_text, head_text):
    """The lines, neither blank nor a comment, that head_text has more of than base_text."""
    def counted(text):
        lines = {}
        for line in (text or "").replace("\r\n", "\n").split("\n"):
            line = line.strip()
            if line and not line.startswith("#"):
                lines[line] = lines.get(line, 0) + 1
        return lines
    base, head = counted(base_text), counted(head_text)
    return sorted(line for line, count in head.items() if count > base.get(line, 0))


def _list_lines(text):
    """The lines of a list, in order, stripped, neither blank nor a comment."""
    return [line.strip() for line in (text or "").replace("\r\n", "\n").split("\n")
            if line.strip() and not line.strip().startswith("#")]


def list_narrowings(path, base_text, head_text):
    """[what] for each way the change makes the list at path (SCOPE_LISTS) leave out more:
    a line it gains; and in .gitignore's syntax (IGNORE_SYNTAX), a "!" line it loses or
    the lines it keeps put in another order, which decides what a "!" line takes back. A
    "!" line gained only takes back more, and a line lost only leaves out less; a list
    taken away leaves out nothing (semgrep.sh refuses to run without its own)."""
    if head_text is None:
        return []
    negating = IGNORE_SYNTAX.search(path)
    gained = [line for line in lines_gained(base_text, head_text) if not (negating and line.startswith("!"))]
    found = []
    if gained:
        found.append("%s gains %d line(s) of what a scan leaves out (%s)" % (
            path, len(gained), "; ".join(g[:80] for g in gained[:3])))
    if not negating:
        return found
    lost = [line for line in lines_gained(head_text, base_text) if line.startswith("!")]
    if lost:
        found.append("%s loses %d line(s) that take back what a line before them left out (%s)" % (
            path, len(lost), "; ".join(g[:80] for g in lost[:3])))
    before, after = _list_lines(base_text), _list_lines(head_text)
    if any(line.startswith("!") for line in before + after):
        # The lines the head keeps of the base's, in the head's order, must come in the
        # base's order: one that does not has moved past a line it was before.
        left = {}
        for line in before:
            left[line] = left.get(line, 0) + 1
        kept = []
        for line in after:
            if left.get(line, 0) > 0:
                kept.append(line)
                left[line] -= 1
        at = 0
        for line in kept:
            while at < len(before) and before[at] != line:
                at += 1
            if at == len(before):
                found.append("%s puts its lines in another order, and in .gitignore's syntax a \"!\" line takes back "
                             "only what a line before it left out (%s)" % (path, line[:80]))
                break
            at += 1
    return found


def scan_scope(text):
    """(the lines between the marks of SCAN_SCOPE_MARK, the lines outside them that carry
    one of the tools' options (SCAN_OPTION), how many marked parts there are, None) - each
    line stripped, neither blank nor a comment - or (the same, the problem) when the marks
    do not pair."""
    inside, outside, parts, problem, within = [], [], 0, None, False
    for number, line in enumerate((text or "").replace("\r\n", "\n").split("\n"), 1):
        mark = SCAN_SCOPE_MARK.match(line)
        if mark:
            if (mark.group(1) == "begin") == within:
                problem = problem or "line %d: a mark that %s" % (number, "begins inside another part" if within
                                                                   else "ends no part")
            within = mark.group(1) == "begin"
            parts += mark.group(1) == "begin"
            continue
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if within:
            inside.append(stripped)
        elif SCAN_OPTION.search(stripped):
            outside.append(stripped)
    if within:
        problem = problem or "the last part has no end"
    return inside, outside, parts, problem


def _outside_scope(text):
    """The lines of a scan script outside its marked parts, stripped, neither blank nor a
    comment."""
    lines, within = [], False
    for line in (text or "").replace("\r\n", "\n").split("\n"):
        mark = SCAN_SCOPE_MARK.match(line)
        if mark:
            within = mark.group(1) == "begin"
            continue
        if not within and line.strip() and not line.strip().startswith("#"):
            lines.append(line.strip())
    return lines


def script_narrowings(path, base_text, head_text):
    """[what] for each way the change to a scan script (SCAN_SCRIPTS) changes what its
    tools read and report: marks that do not pair, or none where the base had some; a
    line of the marked parts gained, lost or moved; and one of the tools' options outside
    them that the base did not have there. Where the base has no marks (before they were
    made), a marked line the base's script does not hold anywhere. A script taken away
    leaves its job failing, not narrowed."""
    if head_text is None:
        return []
    inside, stray, parts, problem = scan_scope(head_text)
    base_inside, base_stray, base_parts, _ = scan_scope(base_text)
    found = []
    if problem:
        found.append("%s's marks of what its tools read (# scan-scope begin, # scan-scope end) do not pair: %s" % (
            path, problem))
    if not parts and base_parts:
        found.append("%s no longer marks the lines that choose what its tools read and report" % path)
    if base_parts:
        if inside != base_inside:
            gained, lost = lines_gained("\n".join(base_inside), "\n".join(inside)), \
                lines_gained("\n".join(inside), "\n".join(base_inside))
            found.append("%s changes the lines that choose what its tools read and report (%s)" % (
                path, "; ".join(["gains %r" % g[:80] for g in gained[:2]] + ["loses %r" % g[:80] for g in lost[:2]])
                or "the same lines in another order"))
        base_outside = base_stray
    else:
        gained = lines_gained(base_text, "\n".join(inside))
        if gained:
            found.append("%s marks lines that choose what its tools read and report which its base does not have "
                         "(%s)" % (path, "; ".join(repr(g[:80]) for g in gained[:3])))
        base_outside = [line for line in _list_lines(base_text) if SCAN_OPTION.search(line)]
    put = lines_gained("\n".join(base_outside), "\n".join(stray))
    if put:
        found.append("%s gives its tools an option outside the lines marked as choosing what they read (%s)" % (
            path, "; ".join(repr(g[:80]) for g in put[:3])))
    return found


def scope_only(path, base_text, head_text):
    """Whether the change to the scan script at path is to its marked lines alone: the
    narrowing's own file, then, not a file it changes besides."""
    return path in SCAN_SCRIPTS and head_text is not None and base_text is not None and \
        scan_scope(base_text)[2] > 0 and _outside_scope(base_text) == _outside_scope(head_text)


def scope_narrowings(inputs):
    """[what] for each way the change narrows what a scan reads: a list of what a scan
    leaves out (SCOPE_LISTS) that leaves out more (list_narrowings), any change of a file
    of SCOPE_FILES, a change of the lines of a scan script that choose what its tools
    read (script_narrowings), an exclusion entry of the analyzer's that no entry of the
    base's already covers, a value of the two lists this cannot read, and any other
    change of a variable of SCOPE_VARIABLE."""
    narrowed = []
    for path, (base_text, head_text) in sorted(inputs["scope_files"].items()):
        if SCOPE_LISTS.search(path):
            narrowed.extend(list_narrowings(path, base_text, head_text))
        elif path in SCAN_SCRIPTS:
            narrowed.extend(script_narrowings(path, base_text, head_text))
        elif base_text != head_text and head_text is not None:
            narrowed.append("%s %s" % (path, "is added" if base_text is None else "is changed"))
    base_variables, head_variables = inputs["variables"]
    base_entries, _ = exclusions(base_variables)
    head_entries, readable = exclusions(head_variables)
    if not readable:
        narrowed.append("a value of %s in a CI file is not one this can read on its line (a block scalar, an alias): "
                        "what the analyzer leaves out cannot be told" % " or ".join(EXCLUSION_VARIABLES))
    wider = [e for e in head_entries if not any(subsumed(e, b) for b in base_entries)]
    if wider:
        narrowed.append("the analyzer's exclusions (%s) gain %s, which the base's do not cover" % (
            " and ".join(EXCLUSION_VARIABLES), ", ".join(repr(e) for e in wider[:5])))
    for name in sorted(set(base_variables) | set(head_variables)):
        if name in EXCLUSION_VARIABLES:
            continue
        before, after = sorted(map(str, base_variables.get(name, []))), sorted(map(str, head_variables.get(name, [])))
        if before != after:
            narrowed.append("%s is %s in the CI files, where the base has %s" % (
                name, ", ".join(repr(v) for v in after) or "not set", ", ".join(repr(v) for v in before) or "it unset"))
    return narrowed


def read_by_a_scan(path):
    """Whether a scan reads path for what it holds: anything but a document, a CI file,
    a file of what the scans read (SCOPE_LISTS, SCOPE_FILES), a baseline, the exceptions
    file, the VEX document or a generated document."""
    return not (path.endswith(".md") or CI_FILE.search(path) or SCOPE_LISTS.search(path) or SCOPE_FILES.search(path)
                or path in [b.replace(os.sep, "/") for b in BASELINES.values()] or path == EXCEPTIONS
                or path == ".gitlab/hmailserver.openvex.json" or path in GENERATED_DOCUMENTS)


def _name(path):
    return path.rsplit("/", 1)[-1].lower()


def reach_problems(moves, base_entries, head_entries, removed=()):
    """What holds a change that puts a source file where semgrep-sast does not read it:
    moved from a path it reads to one it does not; added there while the change deletes
    a source file of the same name from where the scan reads it, a move git did not pair
    because the file was rewritten as well (removed: the paths the change deletes, its
    moves apart); or added (or changed) where only the analyzer's own template leaves it
    out - a build/, dist/, vendor/ or node_modules/ directory below the root, or a
    minified script - which no exclusion of the project's names. moves is [(status, old
    path, new path)]."""
    problems = []
    gone = {}
    for old in removed:
        if SAST_SOURCE.search(old) and unread_reason(old, base_entries) is None:
            gone.setdefault(_name(old), []).append(old)
    for status, old, new in moves:
        if not SAST_SOURCE.search(new):
            continue
        reason = unread_reason(new, head_entries)
        if reason is None:
            continue
        where = "the exclusion" if reason[0] == "listed" else "GitLab's analyzer's own template, pattern"
        if status == "R" and unread_reason(old, base_entries) is None:
            problems.append("%s: moved from %s, which semgrep-sast reads, to where it reads nothing (%s %r): a source "
                            "file stays where the scan reads it, or its place is declared unread by a change of its own "
                            "(SAST_EXCLUDED_PATHS)" % (new, old, where, reason[1]))
        elif status == "A" and gone.get(_name(new)):
            problems.append("%s: added where semgrep-sast reads nothing (%s %r) while this change deletes %s, of the same "
                            "name, from where it reads: a source file moved and rewritten, which git does not pair as a "
                            "move, is put out of the scan's reach as one moved whole is. Keep it where the scan reads it; "
                            "or, if it is not the deleted file, land the deletion and the new file apart, or name it apart; "
                            "or declare its place unread by a change of its own (SAST_EXCLUDED_PATHS)" % (
                                new, where, reason[1], ", ".join(gone[_name(new)][:3])))
        elif reason[0] == "template" and not new.startswith(DECLARED_UNREAD):
            problems.append("%s: GitLab's analyzer reads nothing there (its template leaves out %r at any depth), and no "
                            "exclusion of the project's names the place: put the file where the scan reads it, or declare "
                            "its place unread in SAST_EXCLUDED_PATHS by a change of its own" % (new, reason[1]))
    return problems


def _sarif_note_files(note, text):
    """What a SARIF notification names as files: its locations' URIs, if it has any, and
    the paths its text names with an extension a scan reads (or a directory in them)."""
    named = []
    for location in (note.get("locations") or []) if isinstance(note.get("locations"), list) else []:
        uri = (((location or {}).get("physicalLocation") or {}).get("artifactLocation") or {}).get("uri") \
            if isinstance(location, dict) else None
        if isinstance(uri, str) and uri:
            named.append(uri)
    named.extend(p for p in SARIF_PATH.findall(text) if "/" in p or SAST_SOURCE.search(p))
    return named


def _tree_file(root, path):
    """Whether path names a file of the checkout at root as SARIF_READ_FOR's analyzer names
    one: from the root, with no ./, no leading /, no scheme or drive, no .. step."""
    return bool(path) and not path.startswith("/") and "\\" not in path and ":" not in path and \
        not any(part in (".", "..", "") for part in path.split("/")) and os.path.isfile(os.path.join(root, *path.split("/")))


def sarif_holes(artifacts, paths, root=None):
    """(held, stumbled, None, None) from semgrep-sast's semgrep.sarif: held says, for each
    file of paths, a notification that a rule gave up on it (SARIF_GAVE_UP), and
    stumbled counts those whose parse stumbled (SARIF_PARSE); or (None, None, the
    problem, None) when there is no SARIF to read; or (None, None, None, why) when the
    SARIF says what this cannot read - no run with its invocations, a notification of a
    kind in neither list, or one that gave up on a file and names none - none, with
    root, of the checkout's files as the analyzer names them (_tree_file): a path in
    another form (./src/a.cpp, /builds/.../src/a.cpp) would match no changed file, and
    the hole would pass. Semgrep's wording changed, then (SARIF_READ_FOR), and what it
    gave up on cannot be told until it is read again. GitLab's analyzer fails the scan
    on an error-level notification only, and these are warnings: a report that says
    success can have left a changed file unread by some of its rules."""
    sarif, error = _read_json(os.path.join(artifacts, SEMGREP_SARIF))
    if error:
        return None, None, ("semgrep-sast: %s - the job keeps the SARIF GitLab's analyzer writes beside its report "
                            "(.gitlab/ci/gitlab-security.yml), and without it what the scan gave up on cannot be told"
                            % error), None
    wanted = sorted(set(paths), key=len, reverse=True)
    held, stumbled, invocations = [], set(), 0
    reread = ("Semgrep's notifications were read as analyzer %s writes them (security-gate.py, SARIF_READ_FOR): read the "
              "new analyzer's (build/ci/mirror-semgrep.sh's header) and say in SARIF_GAVE_UP or SARIF_PARSE what this one "
              "means" % SARIF_READ_FOR)
    for run in (sarif.get("runs") or []) if isinstance(sarif, dict) else []:
        for invocation in (run.get("invocations") or []) if isinstance(run, dict) else []:
            invocations += 1
            for note in (invocation.get("toolExecutionNotifications") or []) if isinstance(invocation, dict) else []:
                if not isinstance(note, dict):
                    return None, None, None, "semgrep.sarif holds a notification that is not an object: %r" % (note,)
                kind = (note.get("descriptor") or {}).get("id") if isinstance(note.get("descriptor"), dict) else None
                text = (note.get("message") or {}).get("text") if isinstance(note.get("message"), dict) else None
                kind, text = kind if isinstance(kind, str) else "", text if isinstance(text, str) else ""
                if kind not in SARIF_GAVE_UP and kind not in SARIF_PARSE:
                    return None, None, None, "semgrep.sarif holds a notification of a kind this does not know (%r, %s: " \
                        "%s): %s" % (kind, note.get("level"), text.split("\n")[0][:200], reread)
                files = _sarif_note_files(note, text)
                if kind in SARIF_GAVE_UP and root is not None:
                    files = [f for f in files if _tree_file(root, f)]
                if kind in SARIF_GAVE_UP and not files:
                    return None, None, None, "semgrep.sarif says a rule gave up (%s) and names no file this can find%s in " \
                        "%r: %s" % (kind, "" if root is None else " of the checkout, named from its root,",
                                    text.split("\n")[0][:200], reread)
                named = "\n".join([text] + files)
                for path in wanted:
                    if re.search(r"(?<![\w./-])%s(?=[:\s'\"]|$)" % re.escape(path), named):
                        if kind in SARIF_GAVE_UP:
                            held.append("%s: semgrep-sast gave up on it, so its findings may be missing from the report "
                                        "(%s: %s); run semgrep-sast again, and if it gives up again, make the file one "
                                        "it reads in its time" % (path, kind, text.split("\n")[0][:200]))
                        else:
                            stumbled.add(path)
                        break
    if not invocations:
        return None, None, None, "semgrep.sarif has no run with its invocations, where Semgrep says what it gave up on: " + \
            reread
    return held, sorted(stumbled), None, None


def analyzer_image(text):
    """The image semgrep-sast runs, as gitlab-security.yml names it, or None."""
    block = re.search(r"^semgrep-sast:[ \t]*\n((?:[ \t]+.*\n?|[ \t]*\n)*)", (text or "").replace("\r\n", "\n"), re.MULTILINE)
    if not block:
        return None
    image = re.search(r"^  image:[ \t]*(\S+)[ \t]*$|^    name:[ \t]*(\S+)[ \t]*$", block.group(1), re.MULTILINE)
    return (image.group(1) or image.group(2)) if image else None


def merge_request_evaluate(root, artifacts, exceptions_text, changed, today=None, say=print, own_exceptions_text=None,
                           analyzer_changed=False, not_before=None):
    """The problems that hold a change: semgrep-sast's Critical and High findings in the
    changed files (or anywhere, when the scan's own configuration changed) that
    exceptions_text, the base's, does not accept. When the change re-pins the analyzer
    (analyzer_changed), own_exceptions_text, the change's own copy, also accepts a
    finding in a file the change does not touch. A base's file that cannot be read, or
    an entry of it reviewed after today, accepts nothing, and is said: it holds no
    change of its own, or no change could mend it."""
    today = today or datetime.datetime.now(datetime.timezone.utc).date()
    entries, wrong = check_exceptions(exceptions_text)
    if wrong:
        say("the base's %s cannot be read, so it accepts nothing here: %s" % (EXCEPTIONS, "; ".join(wrong)))
        entries = []
    in_force, wrong = _in_force(entries, today, say)
    for line in wrong:
        say("accepts nothing here: %s" % line)
    whole = sorted(set(changed) & set(SAST_CONFIG))
    scope = None if whole else set(changed)
    outside, own, own_taken = [], None, []
    if whole and analyzer_changed and own_exceptions_text is not None:
        own_entries, wrong = check_exceptions(own_exceptions_text)
        if not wrong:
            # Its lapsed entries are the base's too, or are the change's to drop: said once.
            own_in_force, wrong = _in_force(own_entries, today, lambda line: None)
        if wrong:
            say("this change's own %s cannot be read, so it accepts nothing: %s" % (EXCEPTIONS, "; ".join(wrong)))
        else:
            own = (frozenset(changed), own_in_force.get("semgrep-sast", []))
    found, taken, total = gitlab_sast_scan(root, artifacts, in_force.get("semgrep-sast", []), scope, outside, own, own_taken,
                                           not_before)
    if whole:
        say("this change alters %s, which can change what the scan finds anywhere: every finding in the tree counts%s" % (
            ", ".join(whole), ", and, since it re-pins the analyzer, one in a file it does not touch may be accepted by its "
            "own %s as well as by the base's" % EXCEPTIONS if analyzer_changed else ", each accepted by the base's %s "
                                                                                     "alone" % EXCEPTIONS))
    else:
        say("%d file(s) changed; a finding counts in those alone" % len(scope))
    # The findings outside the changed files are counted, never described: a merge
    # request's job log is public, and those are the tree's unaccepted Critical and
    # High findings, which the release's gate holds and a maintainer reads from the
    # scan's own artifact. A finding in a changed file is the change's, and is named.
    say("semgrep-sast %5d finding(s) in the report; %d accepted by an exception of the base's; %d by the change's "
        "own, outside the files it touches; %d Critical or High outside the changed files, not described here (the "
        "release's gate holds those); %d holding this change" % (
            total, len(taken), len(own_taken), len(outside), len(found)))
    for entry, v in own_taken:
        say("             accepted by the change's own exception until %s (%s): %s - %s, at %s" % (
            entry["expires"], entry["reviewed-by"], entry["finding"], entry["what"], _place(v)))
    return found


def _git(root, *arguments):
    return subprocess.run(["git", "-C", root] + list(arguments), capture_output=True)


def _said(answer):
    return (answer.stderr or answer.stdout or b"").decode("utf-8", "replace").strip()[:300] or "exit %d" % answer.returncode


def _text_at(root, commit, path):
    """(the file's text at commit, or None when the commit has no such file; None) or
    (None, the problem)."""
    shown = _git(root, "show", "%s:%s" % (commit, path))
    if shown.returncode != 0:
        if _git(root, "cat-file", "-e", "%s:%s" % (commit, path)).returncode != 0:
            return None, None
        return None, "git show %s:%s failed: %s" % (commit, path, _said(shown))
    try:
        return shown.stdout.decode("utf-8"), None
    except UnicodeDecodeError as e:
        return None, "%s at %s is not UTF-8: %s" % (path, commit, e)


# ---------------------------------------------------------------- what a change may not add
#
# The source scans pass a finding whose fingerprint is in their baselines (BASELINES),
# and each tool skips a finding where a comment of its own says so. Neither is a
# declaration that the finding is not exploitable, read from the base: a change that
# could add either could silence its own finding. So a baseline only shrinks - a change
# may drop an entry, or give one a reason, and never add a fingerprint or take a reason
# away - and in every file a scan reads, a change may not raise the count of those
# comments: Semgrep's (its marker in the long form and the short, in any case, as
# Semgrep matches it), clang-tidy's (each of its forms that begins a suppression: the
# line, the next line, a block's beginning - not the block's end) and cppcheck's
# (its inline suppression, of a line, a block's beginning, a file or a macro).
# Counting, not reading the lines, lets an edited line keep the marker it had; a file
# git sees renamed keeps the count it had under its old name. Both declarations stay
# where they were: VEX and the exceptions file, read from the base.
#
# The markers are spelt below in pieces, so that this file - which names them, and
# whose own count is held like any other's - does not carry them.
SUPPRESSION_MARKERS = re.compile(
    r"(?<!\w)(?i:%s)|%s(?!END)|%s(?![A-Za-z0-9_]|-end)" % ("no" + "sem", "NO" + "LINT", "cppcheck-" + "suppress"))
# Files whose text names the markers as words and that no scan reads as code:
# documents, the baselines and the exceptions file; and the three files generated from
# documents and page text that every scan leaves out (SAST_EXCLUDED_PATHS in
# .gitlab/ci/gitlab-security.yml, and security-findings.py's GENERATED for the source
# scans: the self-test holds each to both), into which a document's words about the
# markers are copied. A marker in one of them silences nothing.
GENERATED_DOCUMENTS = ("hmailserver/source/Server/Common/Util/HelpPagesData.cpp",
                       "hmailserver/source/Server/Common/Util/PortalLanguagesData.cpp",
                       "hmailserver/source/Server/Common/Util/PortalPageData.cpp")
REGULAR_FILE = ("100644", "100755")


def markers_counted(path):
    """Whether a suppression marker in the file at path can silence a scan: anything but
    a Markdown document, a baseline, the exceptions file or a generated document."""
    return not (path.endswith(".md") or path in [b.replace(os.sep, "/") for b in BASELINES.values()]
                or path == EXCEPTIONS or path in GENERATED_DOCUMENTS)


def suppression_markers(data, path=""):
    """How many suppression markers the file's bytes hold. A file git would call binary
    holds none, unless it is UTF-16 text with its byte order mark, or a source file a scan
    reads by its name (SAST_SOURCE), which a compiler and the scans still read with a NUL
    byte in it."""
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        text = data.decode("utf-16", "replace")
    elif b"\0" in data[:8000] and not SAST_SOURCE.search(path or ""):
        return 0
    else:
        text = data.decode("utf-8", "replace")
    return len(SUPPRESSION_MARKERS.findall(text))


def _raw_diff(output):
    """[(status letter, old mode, new mode, old blob, new blob, old path, new path)] from
    git diff --raw -z --no-abbrev."""
    fields, items, i = output.split(b"\0"), [], 0
    while i < len(fields) and fields[i]:
        meta = fields[i].decode("ascii", "replace")
        parts = meta[1:].split(" ")
        if not meta.startswith(":") or len(parts) != 5 or not parts[4]:
            raise ValueError("not a line of git diff --raw: %r" % meta[:80])
        two = parts[4][0] in "RC"
        if not all(fields[i + 1:i + (3 if two else 2)]) or i + (2 if two else 1) >= len(fields):
            raise ValueError("git diff --raw ends inside an entry: %r" % meta[:80])
        old = fields[i + 1].decode("utf-8", "surrogateescape")
        new = fields[i + 2].decode("utf-8", "surrogateescape") if two else old
        items.append((parts[4][0], parts[0], parts[1], parts[2], parts[3], old, new))
        i += 3 if two else 2
    return items


def _blobs(root, names):
    """({blob: bytes}, None) or (None, the problem): the blobs git cat-file --batch gives."""
    unique = sorted(set(names))
    if not unique:
        return {}, None
    done = subprocess.run(["git", "-C", root, "cat-file", "--batch"], input=("\n".join(unique) + "\n").encode("ascii"),
                          capture_output=True)
    if done.returncode != 0:
        return None, "git cat-file --batch failed: %s" % _said(done)
    out, at, blobs = done.stdout, 0, {}
    try:
        for name in unique:
            end = out.index(b"\n", at)
            header = out[at:end].decode("ascii", "replace").split(" ")
            if len(header) != 3 or header[0] != name or header[1] != "blob":
                return None, "git cat-file --batch gave %r for %s" % (" ".join(header)[:120], name)
            start = end + 1
            blobs[name] = out[start:start + int(header[2])]
            at = start + int(header[2]) + 1
    except ValueError as e:
        return None, "git cat-file --batch's output cannot be read: %s" % e
    return blobs, None


def _changes(root, base):
    """(git diff --raw's entries for each file the change adds, copies, alters, moves or
    retypes, None) or (None, the problem); a move is found at any similarity git gives."""
    raw = _git(root, "diff", "--raw", "-z", "-M", "-l0", "--no-abbrev", "--diff-filter=ACMRT", base, "HEAD")
    if raw.returncode != 0:
        return None, "git diff --raw %s HEAD failed: %s" % (base, _said(raw))
    try:
        return _raw_diff(raw.stdout), None
    except ValueError as e:
        return None, str(e)


def marker_counts(root, base, items=None):
    """([(path, count at base, count at HEAD)] for each file the change adds or alters and
    a scan reads, None) or (None, the problem). A file renamed from one whose markers
    were not counted starts from none. items is _changes', when read already."""
    if items is None:
        items, problem = _changes(root, base)
        if problem:
            return None, problem
    wanted = []
    for status, old_mode, new_mode, old_blob, new_blob, old, new in items:
        if new_mode not in REGULAR_FILE or not markers_counted(new):
            continue
        before = old_blob if (status != "A" and old_mode in REGULAR_FILE and markers_counted(old)) else None
        wanted.append((new, before, new_blob, old))
    blobs, problem = _blobs(root, [b for _, before, after, _ in wanted for b in (before, after) if b])
    if problem:
        return None, problem
    return [(path, suppression_markers(blobs[before], old) if before else 0, suppression_markers(blobs[after], path))
            for path, before, after, old in wanted], None


def _texts_at(root, commit, wanted, pathspecs):
    """({path: text} of the files at commit, under pathspecs, whose path wanted (a
    regular expression) matches, None) or (None, the problem)."""
    listed = _git(root, "ls-tree", "-r", "-z", commit, "--", *pathspecs)
    if listed.returncode != 0:
        return None, "git ls-tree %s failed: %s" % (commit, _said(listed))
    names = {}
    for entry in listed.stdout.split(b"\0"):
        if not entry:
            continue
        meta, _, path = entry.partition(b"\t")
        mode, kind, blob = meta.decode("ascii", "replace").split(" ")
        path = path.decode("utf-8", "surrogateescape")
        if kind == "blob" and wanted.search(path):
            names[path] = blob
    blobs, problem = _blobs(root, list(names.values()))
    if problem:
        return None, problem
    return dict((path, blobs[blob].decode("utf-8", "replace")) for path, blob in names.items()), None


def baseline_problems(path, base_text, head_text):
    """What holds a change in its copy of the baseline at path: an entry the base's does
    not have, and an entry whose reason the change took away."""
    base = findings_module.parse_baseline(base_text or "")
    head = findings_module.parse_baseline(head_text or "")

    def reason(fields):
        return (fields[4] if len(fields) > 4 else "").strip()

    def named(fingerprint, fields):
        return "%s (%s)" % (fingerprint, " ".join(fields[:3])[:200])

    problems = []
    added = [named(fp, fields) for fp, fields in head.items() if fp not in base]
    if added:
        problems.append("%s: %d entr%s the base's copy does not have, %s: a baseline only shrinks, and a finding new to "
                        "it is fixed, or accepted in %s by a change of its own, landed first" % (
                            path, len(added), "y" if len(added) == 1 else "ies", "; ".join(added), EXCEPTIONS))
    lost = [named(fp, fields) for fp, fields in head.items() if fp in base and reason(base[fp]) and not reason(fields)]
    if lost:
        problems.append("%s: %d entr%s whose reason this change took away, %s: a known finding keeps the reason its "
                        "triage gave" % (path, len(lost), "y" if len(lost) == 1 else "ies", "; ".join(lost)))
    return problems


def change_problems(inputs, say=print):
    """What holds a change whatever the scan found: a baseline that grew or lost a
    reason, a file a scan reads with more suppression markers than its base, a
    narrowing of what a scan reads that does not land alone, and a source file put
    where semgrep-sast does not read it."""
    problems = []
    for path, (base_text, head_text) in sorted(inputs["baselines"].items()):
        problems.extend(baseline_problems(path, base_text, head_text))
        say("%s: %d entr(ies) at the base, %d in this change" % (
            path, len(findings_module.parse_baseline(base_text or "")), len(findings_module.parse_baseline(head_text or ""))))
    raised = [(path, before, after) for path, before, after in inputs["markers"] if after > before]
    for path, before, after in raised:
        problems.append("%s: %d comment(s) by which a scan skips a finding, %d at the base: a change adds none - a finding "
                        "is fixed, or accepted in %s by a change of its own, landed first" % (path, after, before, EXCEPTIONS))
    say("suppression markers: %d file(s) this change adds or alters that a scan reads; %d with more than at the base" % (
        len(inputs["markers"]), len(raised)))
    # A narrowing of what a scan reads is a declaration: it lands alone.
    narrowed = scope_narrowings(inputs)
    besides = [p for p in inputs["changed"] if read_by_a_scan(p)]
    besides = [p for p in besides if not scope_only(p, *inputs["scope_files"].get(p, (None, None)))]
    for line in narrowed:
        say("narrows what a scan reads: %s" % line)
    if narrowed and besides:
        problems.append("this change narrows what a scan reads (%s) and changes %d file(s) a scan reads besides (%s): a "
                        "narrowing is a declaration, as an exception is, and lands alone, by a change that touches nothing "
                        "else a scan reads, so that no change hides its own finding" % (
                            "; ".join(narrowed), len(besides), ", ".join(besides[:5]) + (" ..." if len(besides) > 5 else "")))
    elif narrowed:
        say("it changes nothing else a scan reads: the narrowing lands alone, as a declaration")
    else:
        say("what the scans read: nothing narrowed")
    base_entries, _ = exclusions(inputs["variables"][0])
    head_entries, _ = exclusions(inputs["variables"][1])
    problems.extend(reach_problems(inputs["moves"], base_entries, head_entries, inputs.get("removed", ())))
    return problems


def merge_request_inputs(root, base):
    """(inputs, None) or (None, the problem): what the change holds from base to HEAD, as
    the gate reads it: {"changed": the paths it adds, alters or removes, "exceptions":
    the base's exceptions file (none accepts nothing), "own_exceptions": HEAD's, or
    None, "baselines": {path: (the base's text, HEAD's), None for no file}, "markers":
    marker_counts, "scope_files": {path: (the base's text, HEAD's)} of the files of
    SCOPE_LISTS, SCOPE_FILES and SCAN_SCRIPTS it changes, "variables": (the base's
    scope_variables, HEAD's), "moves": [(status, old path, new path)] of the regular files
    it adds, alters or moves, "removed": the paths it deletes, its moves apart, as git
    pairs them, "analyzer": (semgrep-sast's image at the base, at HEAD)}."""
    if not re.match(r"^[0-9a-f]{7,64}$", base or ""):
        return None, ("no base commit (%r): run it with the merge request's CI_MERGE_REQUEST_DIFF_BASE_SHA, or 'auto', "
                      "in a clone that has it (GIT_DEPTH 0)" % base)
    if _git(root, "cat-file", "-e", base + "^{commit}").returncode != 0:
        return None, "the base commit %s is not in this clone: the job needs the history (GIT_DEPTH 0)" % base
    # Every path the change adds, alters or takes away, a move as the two: a file of
    # the scan's configuration (SAST_CONFIG) removed or moved alters the scan as much as
    # one edited, and a path that has gone holds no finding of its own.
    diff = _git(root, "diff", "--name-only", "-z", "--no-renames", base, "HEAD")
    if diff.returncode != 0:
        return None, "git diff %s HEAD failed: %s" % (base, _said(diff))
    inputs = {"changed": [p.decode("utf-8", "surrogateescape") for p in diff.stdout.split(b"\0") if p], "baselines": {}}
    for key, commit in (("exceptions", base), ("own_exceptions", "HEAD")):
        inputs[key], problem = _text_at(root, commit, EXCEPTIONS)
        if problem:
            return None, problem
    # A base from before the file existed accepts nothing.
    if inputs["exceptions"] is None:
        inputs["exceptions"] = "exceptions: []\n"
    for relative in sorted(BASELINES.values()):
        path = relative.replace(os.sep, "/")
        texts = []
        for commit in (base, "HEAD"):
            text, problem = _text_at(root, commit, path)
            if problem:
                return None, problem
            texts.append(text)
        inputs["baselines"][path] = tuple(texts)
    items, problem = _changes(root, base)
    if problem:
        return None, problem
    inputs["markers"], problem = marker_counts(root, base, items)
    if problem:
        return None, problem
    inputs["moves"] = [(status, old, new) for status, old_mode, new_mode, _, _, old, new in items if new_mode in REGULAR_FILE]
    removed = _git(root, "diff", "--name-only", "-z", "-M", "-l0", "--diff-filter=D", base, "HEAD")
    if removed.returncode != 0:
        return None, "git diff --diff-filter=D %s HEAD failed: %s" % (base, _said(removed))
    inputs["removed"] = [p.decode("utf-8", "surrogateescape") for p in removed.stdout.split(b"\0") if p]
    inputs["scope_files"] = {}
    for path in inputs["changed"]:
        if SCOPE_LISTS.search(path) or SCOPE_FILES.search(path) or path in SCAN_SCRIPTS:
            texts = []
            for commit in (base, "HEAD"):
                text, problem = _text_at(root, commit, path)
                if problem:
                    return None, problem
                texts.append(text)
            inputs["scope_files"][path] = tuple(texts)
    # The CI files' variables, and semgrep-sast's image, at HEAD; at the base too when
    # the change touches a CI file, and the same as HEAD's otherwise.
    head_ci, problem = _texts_at(root, "HEAD", CI_FILE, [".gitlab-ci.yml", ".gitlab"])
    if problem:
        return None, problem
    base_ci = head_ci
    if any(CI_FILE.search(p) for p in inputs["changed"]):
        base_ci, problem = _texts_at(root, base, CI_FILE, [".gitlab-ci.yml", ".gitlab"])
        if problem:
            return None, problem
    inputs["variables"] = (scope_variables(base_ci), scope_variables(head_ci))
    inputs["analyzer"] = (analyzer_image(base_ci.get(SAST_JOB_FILE)), analyzer_image(head_ci.get(SAST_JOB_FILE)))
    return inputs, None


def sample_reports():
    """{artifact path: text}: what the four scans leave when they finished and found
    nothing. For the self-tests here and in build/ci/release-chain.py."""
    def report(kind, scanner):
        return json.dumps({"version": "15.2.5", "scan": {"status": "success", "type": kind, "scanner": {"id": scanner}},
                           "vulnerabilities": []})
    reports = {GITLAB_SAST_REPORT: json.dumps({"version": "15.2.5", "scan": {
        "status": "success", "type": "sast", "analyzer": {"id": "semgrep"}, "scanner": {"id": "semgrep"},
        "start_time": "2026-10-06T20:43:57"}, "vulnerabilities": []}),
        SEMGREP_SARIF: json.dumps({"version": "2.1.0", "runs": [{"invocations": [
            {"executionSuccessful": True, "toolExecutionNotifications": []}], "results": []}]})}
    for scan, paths in SOURCE_REPORTS.items():
        for path in paths:
            reports[path] = report("sast", os.path.basename(path)[len("gl-sast-"):-len(".json")])
    for path in DEPENDENCY_REPORTS:
        reports[path] = report("dependency_scanning", os.path.basename(path)[len("gl-dependency-scanning-"):-len(".json")])
    reports[DEPENDENCY_JUNIT] = ('<?xml version="1.0" encoding="UTF-8"?>\n<testsuites>\n<testsuite name="dependency-scan" '
                                 'tests="1" failures="0" skipped="0">\n<testcase classname="dependency-scan.image" '
                                 'name="hmailserver-ci/linux (dpkg and .NET)"><system-out>scanned</system-out></testcase>\n'
                                 '</testsuite>\n</testsuites>\n')
    return reports


# ---------------------------------------------------------------- the self-test

def self_test():
    failures = []
    quiet = lambda line: None

    def expect(condition, what):
        if not condition:
            failures.append(what)

    today = datetime.date(2026, 10, 1)
    work = tempfile.mkdtemp(prefix="security-gate-")
    try:
        root = os.path.join(work, "tree")
        artifacts = os.path.join(work, "artifacts")
        os.makedirs(os.path.join(root, "build", "ci"))
        os.makedirs(os.path.join(root, "src"))
        with open(os.path.join(root, "src", "a.cpp"), "w", encoding="utf-8") as f:
            f.write("int main() {\n  char b[4]; strcpy(b, argv[1]);\n  return atoi(x);\n}\n")

        def fp(tool, rule, path, line):
            f = findings_module.finding(tool, rule, path, line, 0, "High", "m")
            held = findings_module.ROOT
            findings_module.ROOT = root
            findings_module._lines_cache.clear()
            try:
                findings_module.fingerprint_all([f])
            finally:
                findings_module.ROOT = held
                findings_module._lines_cache.clear()
            return f["fingerprint"]

        old = fp("cppcheck", "bufferAccess", "src/a.cpp", 2)
        new = fp("clang-tidy", "cert-err34-c", "src/a.cpp", 3)

        # The semgrep job's holes, which make its report's status "failure" and so hold
        # the release (security-findings.py, read_semgrep), as Semgrep 1.178.0 writes
        # them: a file it could not parse, and a partial parse that lost a block, each
        # at level "warn"; a partial parse of one line is not a hole, nor is one line
        # named eleven times; a timeout, a rule out of memory or out of stack is.
        def span(first, last, key):
            return {key: "build/ci/sign.sh", "start": {"line": first, "col": 1, "offset": 0},
                    "end": {"line": last, "col": 9, "offset": 9}}
        semgrep_json = os.path.join(work, "semgrep.json")
        with open(semgrep_json, "w", encoding="utf-8") as f:
            json.dump({"results": [], "errors": [
                {"code": 3, "level": "warn", "type": "Syntax error", "path": "build/ci/semgrep.sh",
                 "message": "Syntax error at line build/ci/semgrep.sh:1:\n `#!/bin/bash"},
                {"code": 3, "level": "warn", "type": ["PartialParsing", [span(42, 138, "path")]], "path": "build/ci/sign.sh",
                 "spans": [span(42, 138, "file")], "message": "Syntax error at line build/ci/sign.sh:42:\n `set -eu"},
                {"code": 3, "level": "warn", "type": ["PartialParsing", [span(102, 102, "path")]], "path": "build/ci/one.sh",
                 "spans": [span(102, 102, "file")], "message": "Syntax error at line build/ci/one.sh:102:"},
                {"code": 2, "level": "warn", "type": "Timeout", "path": "src/Portal.js", "rule_id": "a-rule",
                 "message": "Timeout when running a-rule on src/Portal.js"},
                {"code": 2, "level": "warn", "type": "Out of memory", "path": "src/Big.js",
                 "message": "Out of memory at line src/Big.js:1:\n Exceeded memory limit"},
                {"code": 2, "level": "warn", "type": "Stack overflow", "path": "src/Deep.py",
                 "message": "Stack overflow at line src/Deep.py:1:\n"},
                {"code": 3, "level": "warn", "type": ["PartialParsing", [span(276, 276, "path")] * 11],
                 "path": "build/ci/same.sh", "spans": [span(276, 276, "file")] * 11, "message": "Syntax error"},
                {"code": 3, "level": "warn", "type": ["PartialParsing", [span(1, 8, "path"), span(5, 10, "path")]],
                 "path": "build/ci/ten.sh", "message": "Syntax error"},
                {"code": 3, "level": "warn", "type": ["PartialParsing", [span(1, 8, "path"), span(20, 22, "path")]],
                 "path": "build/ci/eleven.sh", "message": "Syntax error"}]}, f)
        _, holes = findings_module.read_semgrep(semgrep_json)
        expect(sorted(h["path"] for h in holes) == ["build/ci/eleven.sh", "build/ci/semgrep.sh", "build/ci/sign.sh",
                                                    "src/Big.js", "src/Deep.py", "src/Portal.js"],
               "semgrep's holes are a file it could not parse, a partial parse of 97 lines, one of 11 lines in two spans "
               "given only in its type, a timeout, a rule out of memory and one out of stack - not a partial parse of "
               "one line, of one line named eleven times, or of ten lines in two overlapping spans: %s"
               % sorted((h["path"], h["message"][:60]) for h in holes))
        expect(any("97 lines left unread (42-138)" in h["message"] for h in holes),
               "a partial parse that is a hole says how many lines it left unread, and which")
        expect(any("11 lines left unread (1-8, 20-22)" in h["message"] for h in holes),
               "a partial parse whose spans are only in its type is read from there")
        for scan, name in BASELINES.items():
            with open(os.path.join(root, name), "w", encoding="utf-8") as f:
                f.write("# baseline\n%s\tcppcheck\tbufferAccess\tsrc/a.cpp:2\tm\n" % old)

        def sast(tool, prints, status="success"):
            return {"version": "15.2.5", "scan": {"status": status, "scanner": {"id": tool}, "type": "sast"},
                    "vulnerabilities": [{"id": str(uuid.uuid5(findings_module.ID_NAMESPACE, p)), "name": "rule",
                                         "severity": "Medium", "description": "d",
                                         "identifiers": [{"type": "x", "name": "x", "value": "x"},
                                                         {"type": "hmailserver_fingerprint", "name": p, "value": p}],
                                         "location": {"file": "src/a.cpp", "start_line": 2}} for p in prints]}

        def gitlab(severity="High", status="success", analyzer="semgrep", started="2026-10-01T10:00:00"):
            return {"version": "15.0.4", "scan": {"status": status, "type": "sast", "analyzer": {"id": analyzer},
                                                  "scanner": {"id": "semgrep"}, "start_time": started},
                    "vulnerabilities": [{"id": "random", "name": "strcpy", "severity": severity, "description": "strcpy",
                                         "identifiers": [{"type": "semgrep_id", "name": "c.strcpy", "value": "c.strcpy"}],
                                         "location": {"file": "src/a.cpp", "start_line": 2}}]}

        def deps(severity="High", solution="Upgrade x to 2.", status="success"):
            return {"version": "15.2.5", "scan": {"status": status, "type": "dependency_scanning"},
                    "vulnerabilities": [{"id": "u", "name": "CVE-2026-0001 in zlib 1.3", "severity": severity,
                                         "identifiers": [{"type": "cve", "name": "CVE-2026-0001", "value": "CVE-2026-0001"}],
                                         "location": {"file": "lock", "dependency": {"package": {"name": "zlib"}, "version": "1.3"}},
                                         "solution": solution}] if severity else []}

        junit_ok = ('<testsuites><testsuite><testcase classname="dependency-scan.image" name="i"/>'
                    '<testcase classname="dependency-scan.vendored" name="v"><skipped message="u"/></testcase></testsuite></testsuites>')

        def lay(reports):
            if os.path.isdir(artifacts):
                shutil.rmtree(artifacts)
            for relative, content in reports.items():
                path = os.path.join(artifacts, relative)
                os.makedirs(os.path.dirname(path) or artifacts, exist_ok=True)
                with open(path, "w", encoding="utf-8") as f:
                    f.write(content if isinstance(content, str) else json.dumps(content))

        def clean():
            return {SOURCE_REPORTS["static-analysis"][0]: sast("cppcheck", [old]),
                    SOURCE_REPORTS["static-analysis"][1]: sast("clang-tidy", []),
                    SOURCE_REPORTS["semgrep"][0]: sast("semgrep", [old]),
                    GITLAB_SAST_REPORT: gitlab("Medium"),
                    DEPENDENCY_REPORTS[0]: deps("Medium"),
                    DEPENDENCY_REPORTS[1]: deps(None),
                    DEPENDENCY_JUNIT: junit_ok}

        empty = "# none\nexceptions: []\n"

        def run(reports, exceptions=empty):
            lay(reports)
            held = findings_module.ROOT
            findings_module.ROOT = root
            findings_module._lines_cache.clear()
            try:
                return evaluate(root, artifacts, today, exceptions, say=quiet)
            finally:
                findings_module.ROOT = held
                findings_module._lines_cache.clear()

        expect(run(clean()) == [], "a pipeline whose findings are all in the baselines or below the thresholds passes: %s" % run(clean()))
        expect(run(sample_reports()) == [], "a pipeline whose scans found nothing passes: %s" % run(sample_reports()))

        def refuses(what, reports, needle, exceptions=empty):
            found = run(reports, exceptions)
            if not any(needle in p for p in found):
                failures.append("%s holds the release: %s" % (what, found))

        r = clean(); r[SOURCE_REPORTS["static-analysis"][1]] = sast("clang-tidy", [new])
        refuses("a new clang-tidy finding", r, "fingerprint %s" % new)
        r = clean(); del r[SOURCE_REPORTS["semgrep"][0]]
        refuses("a missing semgrep report", r, "semgrep: there is no")
        r = clean(); r[SOURCE_REPORTS["static-analysis"][0]] = sast("cppcheck", [old], "failure")
        refuses("a failed cppcheck", r, "not success")
        r = clean(); r[GITLAB_SAST_REPORT] = gitlab("High")
        refuses("a High semgrep-sast finding", r, "semgrep-sast: High")
        r = clean(); r[GITLAB_SAST_REPORT] = gitlab("Critical")
        refuses("a Critical semgrep-sast finding", r, "semgrep-sast: Critical")
        r = clean(); del r[GITLAB_SAST_REPORT]
        refuses("no semgrep-sast report", r, "semgrep-sast: there is no")
        # A report GitLab's analyzer did not write, or that began before the pipeline,
        # is no report: a committed copy, or one an analyzer run by hand left.
        r = clean(); r[GITLAB_SAST_REPORT] = gitlab("Medium", analyzer="semgrep-local")
        refuses("a semgrep-sast report another tool wrote", r, "not written by GitLab's Semgrep analyzer")
        created = datetime.datetime(2026, 10, 1, 10, 0, 0)
        for started, holds in (("2026-10-01T09:00:00", True), ("2026-10-01T09:57:00", False), ("2026-10-01T10:04:30", False),
                               ("2026-10-01T10:04:30Z", False), ("2026-10-01T11:04:30+01:00", False), ("yesterday", True)):
            r = clean(); r[GITLAB_SAST_REPORT] = gitlab("Medium", started=started)
            lay(r)
            held = findings_module.ROOT
            findings_module.ROOT = root
            try:
                found = evaluate(root, artifacts, today, empty, say=quiet, not_before=created)
            finally:
                findings_module.ROOT = held
            expect(any("start_time" in p or "before this pipeline was created" in p for p in found) == holds,
                   "a report begun at %s, in a pipeline created at 10:00, %s: %s" % (
                       started, "is not the pipeline's" if holds else "is the pipeline's (within the clock's skew)", found))
        expect(pipeline_created({"CI_PIPELINE_CREATED_AT": "2026-10-06T20:43:57Z"}) == datetime.datetime(2026, 10, 6, 20, 43, 57)
               and pipeline_created({}) is None, "CI_PIPELINE_CREATED_AT is read as UTC, and outside a pipeline is none")
        r = clean(); r[DEPENDENCY_REPORTS[0]] = deps("Critical")
        refuses("a Critical dependency advisory with a fix", r, "CVE-2026-0001 in zlib")
        r = clean(); r[DEPENDENCY_REPORTS[1]] = deps("High", status="failure")
        refuses("an OSV scan that could not run", r, "not success")
        r = clean(); r[DEPENDENCY_JUNIT] = junit_ok.replace('name="i"/>', 'name="i"><skipped message="no dir"/></testcase>')
        refuses("the CI image not scanned", r, "did not scan the CI image")
        r = clean(); r[DEPENDENCY_REPORTS[0]] = deps("High", solution="")
        expect(run(r) == [], "a High advisory with no fixed version is listed, not blocking (SECURITY.md's threshold)")

        def exception(scan, finding, expires="2027-03-01", reviewed="2026-09-26", package=None, reason=None):
            lines = ["exceptions:", "  - scan: %s" % scan, "    finding: %s" % finding]
            if package:
                lines.append("    package: %s" % package)
            lines += ["    what: 'the thing'",
                      "    reason: %s" % (reason or "Read in the code: the input is a fixed literal, never a peer's."),
                      "    reviewed-by: Progressiverobot", "    reviewed: %s" % reviewed, "    expires: %s" % expires]
            return "\n".join(lines) + "\n"

        r = clean(); r[SOURCE_REPORTS["static-analysis"][1]] = sast("clang-tidy", [new])
        expect(run(r, exception("static-analysis", new)) == [], "a new finding accepted by an exception passes")
        refuses("a finding whose exception has expired", r, "fingerprint %s" % new, exception("static-analysis", new, expires="2026-09-30"))
        refuses("an exception for another scan", r, "fingerprint %s" % new, exception("semgrep", new))
        sast_fp = None
        r = clean(); r[GITLAB_SAST_REPORT] = gitlab("High")
        for p in run(r):
            m = re.search(r"fingerprint ([0-9a-f]{20})$", p)
            if m:
                sast_fp = m.group(1)
        expect(sast_fp is not None, "a semgrep-sast finding is printed with its fingerprint")
        if sast_fp:
            expect(run(r, exception("semgrep-sast", sast_fp)) == [], "a semgrep-sast finding accepted by its fingerprint passes")
        r = clean(); r[DEPENDENCY_REPORTS[0]] = deps("Critical")
        expect(run(r, exception("dependency-scan", "CVE-2026-0001", package="zlib")) == [], "an advisory accepted for its package passes")
        refuses("an advisory accepted for another package", r, "CVE-2026-0001 in zlib",
                exception("dependency-scan", "CVE-2026-0001", package="openssl"))

        # The file itself.
        refuses("an exception without an expiry", clean(), "it has no expires",
                exception("semgrep", new).replace("    expires: 2027-03-01\n", ""))
        refuses("an exception for more than a year", clean(), "reviewed again within a year",
                exception("semgrep", new, expires="2027-12-01"))
        refuses("an exception with no reason worth the name", clean(), "says too little", exception("semgrep", new, reason="ok"))
        refuses("an exception reviewed in the future", clean(), "after today", exception("semgrep", new, reviewed="2026-12-01", expires="2027-01-01"))
        refuses("an exception with a field the file does not have", clean(), "is not a field",
                exception("semgrep", new).replace("    what:", "    note: x\n    what:"))
        refuses("a dependency exception without its package", clean(), "names the package",
                exception("dependency-scan", "CVE-2026-0001"))
        refuses("an exception for a scan that does not exist", clean(), "not one of", exception("codeql", new))
        refuses("YAML outside the subset", clean(), "is YAML this file does not use",
                exception("semgrep", new).replace("    what: 'the thing'", "    what: |"))
        refuses("a file that is not the exceptions file", clean(), "begins with 'exceptions:'", "foo: bar\n")
        refuses("the same finding twice", clean(), "names the same finding", exception("semgrep", new) + exception("semgrep", new).split("\n", 1)[1])
        lay(clean())
        expect(any("is missing" in p for p in evaluate(os.path.join(work, "nowhere"), artifacts, today, None, say=quiet)),
               "a tree without the exceptions file holds the release")
        with open(os.path.join(ROOT, EXCEPTIONS), encoding="utf-8") as f:
            _, own = check_exceptions(f.read())
        expect(own == [], "the tree's own %s is well formed: %s" % (EXCEPTIONS, own))

        # A merge request's gate: semgrep-sast alone, in the files it changes.
        def gate(reports, changed, exceptions=empty):
            lay(reports)
            held = findings_module.ROOT
            findings_module.ROOT = root
            findings_module._lines_cache.clear()
            try:
                return merge_request_evaluate(root, artifacts, exceptions, changed, today, say=quiet)
            finally:
                findings_module.ROOT = held
                findings_module._lines_cache.clear()

        critical = {GITLAB_SAST_REPORT: gitlab("Critical"), SEMGREP_SARIF: sample_reports()[SEMGREP_SARIF]}
        found = gate(critical, ["src/a.cpp"])
        expect(any(p.startswith("semgrep-sast: Critical") for p in found),
               "a merge request that changes a file with a Critical finding is held: %s" % found)
        found = gate({GITLAB_SAST_REPORT: gitlab("High")}, ["src/a.cpp", "README.md"])
        expect(any(p.startswith("semgrep-sast: High") for p in found), "a High finding in a changed file holds it: %s" % found)
        expect(gate(critical, ["src/other.cpp"]) == [],
               "a Critical finding in a file the merge request does not change is the release's, not the merge request's")
        # ... and is counted in the public log, never described.
        said = []
        lay(critical)
        held_root = findings_module.ROOT
        findings_module.ROOT = root
        findings_module._lines_cache.clear()
        try:
            merge_request_evaluate(root, artifacts, empty, ["src/other.cpp"], today, say=said.append)
        finally:
            findings_module.ROOT = held_root
            findings_module._lines_cache.clear()
        expect(any("1 Critical or High outside the changed files, not described here" in s for s in said),
               "a finding outside the changed files is counted: %s" % said)
        expect(not any("src/a.cpp" in s or "fingerprint" in s or "strcpy" in s for s in said),
               "a finding outside the changed files is not described in the merge request's log: %s" % said)
        for config in (".gitlab-ci.yml", ".gitlab/ci/mr-checks.yml"):
            found = gate(critical, ["src/other.cpp", config])
            expect(any(p.startswith("semgrep-sast: Critical") for p in found),
                   "a merge request that changes %s, which can change the scan or this gate, answers for every finding: %s"
                   % (config, found))
        found = gate(critical, ["src/other.cpp", ".gitlab/ci/gitlab-security.yml"])
        expect(any(p.startswith("semgrep-sast: Critical") for p in found),
               "a merge request that changes the scan itself answers for every finding: %s" % found)
        found = gate(critical, ["src/other.cpp", ".gitlab/sast-ruleset.toml"])
        expect(any(p.startswith("semgrep-sast: Critical") for p in found),
               "so does one that adds or changes GitLab's custom ruleset: %s" % found)
        expect(gate({GITLAB_SAST_REPORT: gitlab("Medium")}, ["src/a.cpp"]) == [], "a Medium finding holds nothing")
        if sast_fp:
            expect(gate(critical, ["src/a.cpp"], exception("semgrep-sast", sast_fp)) == [],
                   "a Critical finding the base's exceptions accept by its fingerprint passes")
            found = gate(critical, ["src/a.cpp"], exception("semgrep-sast", sast_fp, expires="2026-09-30"))
            expect(any(p.startswith("semgrep-sast: Critical") for p in found), "an expired exception accepts nothing: %s" % found)
        found = gate({}, ["src/a.cpp"])
        expect(any("semgrep-sast: there is no" in p for p in found), "no report holds the merge request: %s" % found)
        found = gate({GITLAB_SAST_REPORT: gitlab("Critical", status="failure")}, ["src/a.cpp"])
        expect(any("not success" in p for p in found), "a scan that did not finish holds it: %s" % found)
        # A base's exceptions file that cannot be read, or an entry of it reviewed after
        # today, accepts nothing, and holds no change of its own: the change that mends
        # it would be held by it.
        said = []
        lay(critical)
        findings_module.ROOT, held_root = root, findings_module.ROOT
        try:
            found = merge_request_evaluate(root, artifacts, "foo: bar\n", ["src/a.cpp"], today, say=said.append)
            expect(any(p.startswith("semgrep-sast: Critical") for p in found) and not any("cannot be read" in p for p in found)
                   and any("cannot be read, so it accepts nothing" in s for s in said),
                   "a base's exceptions file the gate cannot read accepts nothing, and is said, not held: %s %s" % (found, said))
            found = merge_request_evaluate(root, artifacts, "foo: bar\n", ["README.md"], today, say=said.append)
            expect(found == [], "a change that finds nothing to accept passes a base whose exceptions cannot be read: %s" % found)
            if sast_fp:
                future = exception("semgrep-sast", sast_fp, reviewed="2026-12-01", expires="2027-01-01")
                found = merge_request_evaluate(root, artifacts, future, ["src/a.cpp"], today, say=said.append)
                expect(any(p.startswith("semgrep-sast: Critical") for p in found) and not any("after today" in p for p in found),
                       "an entry of the base's reviewed after today accepts nothing, and is said, not held: %s" % found)
        finally:
            findings_module.ROOT = held_root

        # And from git, as the job runs it: the base's exceptions, never the head's.
        repo = os.path.join(work, "merge-request")
        os.makedirs(os.path.join(repo, "src"))
        os.makedirs(os.path.join(repo, ".gitlab"))
        quiet_git = ["-c", "core.autocrlf=false", "-c", "user.name=self-test", "-c", "user.email=self-test@example.invalid",
                     "-c", "commit.gpgsign=false"]

        def commit(files, message):
            for relative, text in files.items():
                with open(os.path.join(repo, *relative.split("/")), "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
            subprocess.run(["git", "-C", repo] + quiet_git + ["add", "-A"], check=True, capture_output=True)
            subprocess.run(["git", "-C", repo] + quiet_git + ["commit", "-q", "--no-verify", "-m", message], check=True,
                           capture_output=True)
            return subprocess.run(["git", "-C", repo, "rev-parse", "HEAD"], check=True, capture_output=True,
                                  text=True).stdout.strip()

        source = "int main() {\n  char b[4]; strcpy(b, argv[1]);\n  return atoi(x);\n}\n"
        subprocess.run(["git", "init", "-q", repo], check=True, capture_output=True)
        accepting = exception("semgrep-sast", sast_fp or "0" * 20)

        def verdict(base):
            lay(critical)
            held = findings_module.ROOT
            findings_module.ROOT = repo
            findings_module._lines_cache.clear()
            findings_module._top_level = None
            try:
                return merge_request_main(repo, artifacts, base, None, today, say=quiet)
            finally:
                findings_module.ROOT = held
                findings_module._lines_cache.clear()
                findings_module._top_level = None

        base = commit({"src/a.cpp": "int main() { return 0; }\n", "src/other.cpp": "int f();\n", EXCEPTIONS: empty}, "base")
        commit({"src/a.cpp": source, EXCEPTIONS: accepting}, "a change that accepts its own finding")
        expect(verdict(base) == 1, "a merge request whose own copy of the exceptions accepts its finding is still held")
        subprocess.run(["git", "-C", repo, "reset", "-q", "--hard", base], check=True, capture_output=True)
        accepted_base = commit({EXCEPTIONS: accepting}, "the exception, landed first")
        commit({"src/a.cpp": source}, "the change after it")
        if sast_fp:
            expect(verdict(accepted_base) == 0, "the finding the base's exceptions accept lets the merge request land")
        subprocess.run(["git", "-C", repo, "reset", "-q", "--hard", base], check=True, capture_output=True)
        with open(os.path.join(repo, "src", "a.cpp"), "w", encoding="utf-8", newline="\n") as f:
            f.write(source)
        subprocess.run(["git", "-C", repo] + quiet_git + ["add", "-A"], check=True, capture_output=True)
        subprocess.run(["git", "-C", repo] + quiet_git + ["commit", "-q", "--no-verify", "-m", "the flagged line, first"],
                       check=True, capture_output=True)
        after_flagged = subprocess.run(["git", "-C", repo, "rev-parse", "HEAD"], check=True, capture_output=True,
                                       text=True).stdout.strip()
        commit({"src/other.cpp": "int f();\nint g();\n"}, "a change elsewhere")
        expect(verdict(after_flagged) == 0, "a merge request that does not touch the flagged file is not held by it")
        expect(verdict("0" * 40) == 2, "a base the clone does not have cannot be checked, and says so")
        expect(verdict("") == 2, "no base cannot be checked, and says so")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    failures.extend(change_base_failures())
    failures.extend(change_gate_failures())
    failures.extend(path_guard_failures())

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def path_guard_failures():
    """The self-test's cases for the lines at the top that keep this script's directory
    off sys.path: a copy of the gate, run beside files named as the modules it imports,
    runs none of them."""
    failures = []
    here = os.path.dirname(os.path.realpath(__file__))
    if any(os.path.normcase(os.path.realpath(p or os.curdir)) == os.path.normcase(here) for p in sys.path):
        failures.append("this script's directory is on sys.path: %s" % here)
    work = tempfile.mkdtemp(prefix="security-gate-path-")
    try:
        ci = os.path.join(work, "build", "ci")
        os.makedirs(ci)
        for name in ("security-gate.py", "security-findings.py"):
            shutil.copyfile(os.path.join(here, name), os.path.join(ci, name))
        for module in ("argparse", "datetime", "fnmatch", "json", "re", "shutil", "subprocess", "tempfile", "uuid",
                       "collections", "contextlib", "hashlib", "io"):
            with open(os.path.join(ci, module + ".py"), "w", encoding="utf-8") as f:
                f.write("import sys\nprint('SHADOWED: %s')\nsys.exit(77)\n" % module)
        done = subprocess.run([sys.executable, os.path.join(ci, "security-gate.py"), "--help"], cwd=work,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        said = done.stdout.decode("utf-8", "replace")
        if done.returncode != 0 or "SHADOWED" in said:
            failures.append("a copy of the gate beside files named as its modules ran one of them (exit %d): %s" % (
                done.returncode, said[-400:]))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return failures


# ---------------------------------------------------------------- the change's base
#
# The commit a change is judged against, for this gate (--merge-request auto) and for
# build/ci/mr-dependency-check.py (--base auto): the change is what lies between it and
# HEAD, and the exceptions and the VEX document are read as it has them. In a merge
# request's pipeline (CI_PIPELINE_SOURCE merge_request_event) it is GitLab's
# CI_MERGE_REQUEST_DIFF_BASE_SHA, the target branch where the merge request left it -
# taken only there, and only when it is an ancestor of HEAD and of the target branch
# fetched by name, so that no base off the target branch, such as a commit of the
# change's own, is taken. A maintainer's batch* branch runs no merge request pipeline
# (the workflow rules of .gitlab-ci.yml): its branch pipeline is the one GitLab holds its
# merge request to, and there the base is where the branch leaves master - master
# fetched by name, as release-chain.py's check_tag fetches it, and the merge base of
# HEAD and it. Anything else cannot be judged, and says so (exit 2, never a pass).
#
# GitLab lets a variable a CI file defines stand in for one of its own, and every
# CI_* variable these read is one. A CI file of the change that defines a CI_* or
# GITLAB_* variable (ci_variable_definitions) holds it (auto_base): the base, and
# which pipeline this is, would be the change's to say.

TARGET_BRANCH = "master"
TARGET_REF = "refs/remotes/origin/" + TARGET_BRANCH
FULL_SHA = re.compile(r"^[0-9a-f]{40}(?:[0-9a-f]{24})?$")
BRANCH_NAME = re.compile(r"^(?!-)(?!.*\.\.)[A-Za-z0-9._/-]+$")
# GitLab's predefined variables, as a CI file would define one: a key of a mapping.
CI_VARIABLE = re.compile(r"""(?:^[ \t]*(?:-[ \t]+)?|[{,][ \t]*)["']?((?:CI|GITLAB)_[A-Z0-9_]+)["']?[ \t]*:(?=[ \t]|$)""",
                         re.MULTILINE)
# The ones a CI file may set, which GitLab reads as settings and the gates never read:
# CI_DEBUG_SERVICES, which shows a service container's log in its job's
# (.gitlab/ci/container-image.yml).
CI_VARIABLES_ALLOWED = ("CI_DEBUG_SERVICES",)


def ci_variable_definitions(texts):
    """[(path, name)] for each CI_* or GITLAB_* variable the CI files texts ({path: text})
    define, but CI_VARIABLES_ALLOWED."""
    found = []
    for path in sorted(texts):
        for match in CI_VARIABLE.finditer(_uncommented(texts[path] or "")):
            if match.group(1) not in CI_VARIABLES_ALLOWED:
                found.append((path, match.group(1)))
    return found


def change_base(root, environ=None, run=None):
    """(base commit, how it was found, None), or (None, None, the problem)."""
    environ = os.environ if environ is None else environ
    run = run or (lambda arguments: _git(root, *arguments))

    def said(answer):
        return (answer.stderr or answer.stdout or b"").decode("utf-8", "replace").strip()[:300] or "exit %d" % answer.returncode

    merge_request = environ.get("CI_PIPELINE_SOURCE") == "merge_request_event"
    given = (environ.get("CI_MERGE_REQUEST_DIFF_BASE_SHA") or "").strip()
    if not merge_request and (given or environ.get("CI_MERGE_REQUEST_IID")):
        # GitLab gives these to a merge request's pipeline alone: here a CI file or a
        # variable has set them, and the base they would name is not taken.
        return None, None, ("CI_MERGE_REQUEST_DIFF_BASE_SHA or CI_MERGE_REQUEST_IID is set in a pipeline that is not a "
                            "merge request's (CI_PIPELINE_SOURCE %r): its base cannot be told"
                            % environ.get("CI_PIPELINE_SOURCE"))
    if merge_request and not given:
        # A merge request may target another branch than master: its base is GitLab's
        # to give, never guessed.
        return None, None, "a merge request's pipeline without CI_MERGE_REQUEST_DIFF_BASE_SHA: its base cannot be told"
    if given and not FULL_SHA.match(given):
        return None, None, "CI_MERGE_REQUEST_DIFF_BASE_SHA is %r, not a commit" % given
    target = (environ.get("CI_MERGE_REQUEST_TARGET_BRANCH_NAME") or "").strip() if merge_request else TARGET_BRANCH
    if not BRANCH_NAME.match(target):
        return None, None, "a merge request's pipeline whose target branch is %r: its base cannot be told" % target
    target_ref = "refs/remotes/origin/" + target
    shallow = run(["rev-parse", "--is-shallow-repository"])
    if shallow.returncode != 0:
        return None, None, "this is not a git checkout (git rev-parse: %s)" % said(shallow)
    if shallow.stdout.decode("utf-8", "replace").strip() == "true":
        # A shallow boundary can hide where the branch left master.
        return None, None, "this clone is shallow: the job needs the history (GIT_DEPTH 0) to find where it leaves %s" % target
    fetched = run(["fetch", "--no-tags", "origin", "+refs/heads/%s:%s" % (target, target_ref)])
    if fetched.returncode != 0:
        return None, None, "%s could not be fetched from origin: %s; retry the job" % (target, said(fetched))
    tip = run(["rev-parse", "--verify", "--quiet", target_ref + "^{commit}"])
    at = tip.stdout.decode("utf-8", "replace").strip() if tip.returncode == 0 else "?"
    if merge_request:
        if run(["cat-file", "-e", given + "^{commit}"]).returncode != 0:
            return None, None, "the merge request's diff base %s is not in this clone" % given
        for where in ("HEAD", target_ref):
            if run(["merge-base", "--is-ancestor", given, where]).returncode != 0:
                return None, None, ("the merge request's diff base %s is not an ancestor of %s: a base off %s, as %s has "
                                    "it now (%s), is not taken - a merge request from a fork is judged in a pipeline of "
                                    "the project's own (SECURITY.md, 'Every change')" % (
                                        given, "HEAD" if where == "HEAD" else target, target, "origin", at))
        return given, "the merge request's diff base, CI_MERGE_REQUEST_DIFF_BASE_SHA, an ancestor of HEAD and of %s " \
                      "fetched now at %s" % (target, at), None
    merged = run(["merge-base", "HEAD", target_ref])
    base = merged.stdout.decode("utf-8", "replace").strip() if merged.returncode == 0 else ""
    if not FULL_SHA.match(base):
        return None, None, "HEAD and %s have no merge base (git merge-base: %s)" % (target, said(merged))
    return base, "where HEAD leaves %s: git merge-base HEAD %s, with %s fetched now at %s%s" % (
        target, target_ref, target, at, " (its tip)" if at == base else ""), None


def auto_base(root, say=print, environ=None):
    """(base, 0) for --merge-request auto and --base auto, said as "The base: ...", which
    build/land-via-mr.py reads back from the job's log; or (None, 1) when a CI file of the
    change defines a variable GitLab gives (ci_variable_definitions), or (None, 2) when no
    base can be told (change_base)."""
    texts, problem = _texts_at(root, "HEAD", CI_FILE, [".gitlab-ci.yml", ".gitlab"])
    if problem:
        say("CANNOT CHECK: " + problem)
        return None, 2
    defined = ci_variable_definitions(texts)
    if defined:
        for path, name in defined:
            say("HELD: %s defines %s, a variable GitLab gives a pipeline: a CI file's definition stands in for GitLab's, "
                "and would say which pipeline this is and what the change's base is" % (path, name))
        return None, 1
    base, how, problem = change_base(root, environ)
    if problem:
        say("CANNOT CHECK: " + problem)
        return None, 2
    say("The base: %s, %s." % (base, how))
    return base, 0


# ---------------------------------------------------------------- the base's copy of the gates
#
# The gates run the change's own copies of their scripts, and an edit of one - a
# fingerprint that maps a new finding onto an accepted one, a library left out of
# dependency-scan's list - would judge the change that makes it. So when a change
# alters the gates' code (GATE_CODE), each gate runs the base's copy too, over a
# checkout of the change's head in which every file of GATE_CODE is the base's, and the
# change passes only when both copies pass it (combined). The change's CI files still
# say what runs: this is against an edit of the code that the change's own gate would
# not see, not against a change that edits the jobs themselves, which SECURITY.md says.
#
# Whether the base's copy runs is read from the code that runs, never from a variable:
# the checkout's files of GATE_CODE against the base's (running_gate_code). The base's
# copy runs in a worktree whose GATE_CODE is the base's, where none differs, and so runs
# no copy of its own; and nothing a CI file or a project variable sets can stop it. Each
# gate says "The base's copy of <script> exited <code>." in its log, which
# build/land-via-mr.py requires of a batch that changes GATE_CODE.

GATE_CODE = ("build/ci/security-gate.py", "build/ci/security-findings.py", "build/ci/mr-dependency-check.py",
             "build/ci/dependency-scan.py", "build/ci/osv-offline-db.py")
# Where base_copy_verdict makes the base's copy's worktree: <temp>/gate-base-copy-*/tree.
BASE_COPY_DIR = "gate-base-copy-"


def in_base_copy(root):
    """Whether root is a worktree base_copy_verdict made for a base's copy. Not what
    decides whether a copy runs - a checkout put at such a path decides nothing - but a
    bound: a base's copy that still finds the gates' code not the base's stops there,
    unable to check, and never runs a copy of its own."""
    here = os.path.abspath(root)
    return os.path.basename(here) == "tree" and os.path.basename(os.path.dirname(here)).startswith(BASE_COPY_DIR)


def running_gate_code(root, base):
    """([the files of GATE_CODE whose copy in the checkout at root - the code that runs -
    is not the base's], None), or (None, the problem). A file one has and the other lacks
    differs; line endings do not."""
    differ = []
    for path in GATE_CODE:
        at_base = None
        if _git(root, "cat-file", "-e", "%s:%s" % (base, path)).returncode == 0:
            shown = _git(root, "show", "%s:%s" % (base, path))
            if shown.returncode != 0:
                return None, "git show %s:%s failed: %s" % (base, path, _said(shown))
            at_base = shown.stdout.replace(b"\r\n", b"\n")
        try:
            with open(os.path.join(root, *path.split("/")), "rb") as f:
                here = f.read().replace(b"\r\n", b"\n")
        except FileNotFoundError:
            here = None
        except OSError as e:
            return None, "%s cannot be read: %s" % (path, e)
        if here != at_base:
            differ.append(path)
    return differ, None


def base_copy_verdict(root, base, entry, arguments, say=print, environ=None):
    """None when the checkout's GATE_CODE is the base's (running_gate_code) - a change
    that leaves it alone, or the base's own copy running - or when the base has no copy
    of entry; else the exit code of the base's copy of entry, run with arguments from a
    worktree of HEAD in which each file of GATE_CODE is the base's (one the base lacks
    is taken away). No variable stops it: environ is the environment it runs in."""
    environ = os.environ if environ is None else environ
    differ, problem = running_gate_code(root, base)
    if problem:
        say("CANNOT CHECK: the gates' code against the base's: %s" % problem)
        return 2
    if not differ:
        return None
    if in_base_copy(root):
        say("CANNOT CHECK: this runs as the base's copy, in the worktree made for it, and finds the gates' code not the "
            "base's (%s): it runs no copy of its own" % ", ".join(differ))
        return 2
    if _git(root, "cat-file", "-e", "%s:%s" % (base, entry)).returncode != 0:
        say("The base has no %s: the change's copy alone judges it." % entry)
        return None
    work = tempfile.mkdtemp(prefix=BASE_COPY_DIR)
    tree = os.path.join(work, "tree")
    try:
        added = _git(root, "worktree", "add", "--detach", "--quiet", tree, "HEAD")
        if added.returncode != 0:
            say("CANNOT CHECK: a worktree of HEAD for the base's copy of the gates could not be made: %s" % _said(added))
            return 2
        for path in GATE_CODE:
            target = os.path.join(tree, *path.split("/"))
            if _git(root, "cat-file", "-e", "%s:%s" % (base, path)).returncode == 0:
                shown = _git(root, "show", "%s:%s" % (base, path))
                if shown.returncode != 0:
                    say("CANNOT CHECK: git show %s:%s failed: %s" % (base, path, _said(shown)))
                    return 2
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as f:
                    f.write(shown.stdout)
            elif os.path.exists(target):
                os.remove(target)
        say("This change alters the gates' code (%s): the base's copy of %s judges it too, over its head." % (
            ", ".join(differ), entry))
        done = subprocess.run([sys.executable, os.path.join(tree, *entry.split("/"))] + list(arguments), cwd=tree,
                              env=dict(environ), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for line in done.stdout.decode("utf-8", "replace").splitlines():
            say("  base's copy: " + line)
        say("The base's copy of %s exited %d." % (entry, done.returncode))
        return done.returncode
    finally:
        _git(root, "worktree", "remove", "--force", tree)
        shutil.rmtree(work, ignore_errors=True)
        _git(root, "worktree", "prune")


def combined(own, base_copy):
    """The verdict of a change both copies of a gate judged: held when either holds it (1),
    unreadable when neither holds it and either could not read it (2), else passed."""
    codes = [c for c in (own, base_copy) if c is not None]
    if 1 in codes:
        return 1
    if any(c != 0 for c in codes):
        return 2
    return 0


def change_base_failures():
    """The self-test's cases for change_base, auto_base's CI variables and the base's copy
    of the gates, against repositories made here: an origin with master, a clone on a
    branch after it, a shallow clone and a merge request's environment. Offline: origin
    is a directory."""
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    work = tempfile.mkdtemp(prefix="security-gate-base-")
    quiet_git = ["-c", "core.autocrlf=false", "-c", "user.name=self-test", "-c", "user.email=self-test@example.invalid",
                 "-c", "commit.gpgsign=false", "-c", "init.defaultBranch=master", "-c", "protocol.file.allow=always"]

    def g(where, *arguments):
        return subprocess.run(["git", "-C", where] + quiet_git + list(arguments), check=True, capture_output=True,
                              text=True).stdout.strip()

    def commit(where, name, text):
        path = os.path.join(where, *name.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        g(where, "add", "-A")
        g(where, "commit", "-q", "--no-verify", "-m", name)
        return g(where, "rev-parse", "HEAD")

    try:
        origin = os.path.join(work, "origin")
        os.makedirs(origin)
        subprocess.run(["git"] + quiet_git + ["init", "-q", origin], check=True, capture_output=True)
        g(origin, "checkout", "-q", "-b", "master")
        forked = commit(origin, "a.txt", "one\n")
        clone = os.path.join(work, "clone")
        subprocess.run(["git"] + quiet_git + ["clone", "-q", "--no-local", origin, clone], check=True, capture_output=True)
        g(clone, "checkout", "-q", "-b", "batch99")
        own = commit(clone, "b.txt", "the batch's change\n")
        commit(clone, "b2.txt", "and another\n")
        # master moves on after the branch left it: the base is still where it left.
        moved = commit(origin, "c.txt", "master's later change\n")
        environ = {"CI_PIPELINE_SOURCE": "push", "CI_COMMIT_BRANCH": "batch99"}
        base, how, problem = change_base(clone, environ)
        expect(problem is None and base == forked, "a batch branch's base is where it leaves master, fetched now: %r %r %r"
               % (base, how, problem))
        expect(problem is None and moved in (how or ""), "and the base says which master it read: %r" % how)
        expect(g(clone, "rev-parse", TARGET_REF) == moved, "master is fetched by name into %s" % TARGET_REF)
        merge_request = {"CI_PIPELINE_SOURCE": "merge_request_event", "CI_MERGE_REQUEST_TARGET_BRANCH_NAME": "master"}
        expect(change_base(clone, dict(merge_request, CI_MERGE_REQUEST_DIFF_BASE_SHA=forked))[:1] == (forked,),
               "a merge request's pipeline takes CI_MERGE_REQUEST_DIFF_BASE_SHA, on master and below HEAD: %r"
               % (change_base(clone, dict(merge_request, CI_MERGE_REQUEST_DIFF_BASE_SHA=forked)),))
        # A base a CI file could name: a commit of the change's own, which no merge
        # request's diff starts from, and the variable in a pipeline of another kind.
        found = change_base(clone, dict(merge_request, CI_MERGE_REQUEST_DIFF_BASE_SHA=own))
        expect(found[0] is None and "not an ancestor of master" in (found[2] or ""),
               "a diff base off the target branch, a commit of the change's own, is refused: %r" % (found,))
        found = change_base(clone, dict(environ, CI_MERGE_REQUEST_DIFF_BASE_SHA=forked))
        expect(found[0] is None and "not a merge request's" in (found[2] or ""),
               "CI_MERGE_REQUEST_DIFF_BASE_SHA in a pipeline that is not a merge request's is refused: %r" % (found,))
        expect(change_base(clone, dict(merge_request, CI_MERGE_REQUEST_DIFF_BASE_SHA="HEAD~1"))[2] is not None,
               "a CI_MERGE_REQUEST_DIFF_BASE_SHA that is not a commit is refused")
        expect(change_base(clone, dict(merge_request, CI_MERGE_REQUEST_DIFF_BASE_SHA="a" * 40))[2] is not None,
               "a CI_MERGE_REQUEST_DIFF_BASE_SHA the clone does not have is refused")
        expect(change_base(clone, {"CI_PIPELINE_SOURCE": "merge_request_event"})[2] is not None,
               "a merge request's pipeline without its diff base is refused, not judged against master")
        expect(change_base(clone, dict(merge_request, CI_MERGE_REQUEST_DIFF_BASE_SHA=forked,
                                       CI_MERGE_REQUEST_TARGET_BRANCH_NAME="--upload-pack=x"))[2] is not None,
               "a target branch that is not a branch's name is refused")
        # auto_base: a CI file of the change that defines a variable GitLab gives holds it.
        said = []
        commit(clone, ".gitlab/ci/x.yml", "variables:\n  CI_MERGE_REQUEST_DIFF_BASE_SHA: %s\n" % own)
        expect(auto_base(clone, said.append, environ) == (None, 1) and any("defines CI_MERGE_REQUEST_DIFF_BASE_SHA" in s
                                                                           for s in said),
               "a CI file that defines CI_MERGE_REQUEST_DIFF_BASE_SHA holds the change: %s" % said)
        commit(clone, ".gitlab/ci/x.yml", "job:\n  variables:\n    CI_DEBUG_SERVICES: \"true\"\n  rules:\n"
                                          "    - if: $CI_COMMIT_TAG\n# CI_PIPELINE_SOURCE: push\n")
        said = []
        found = auto_base(clone, said.append, environ)
        expect(found == (forked, 0) and any(s.startswith("The base: %s" % forked) for s in said),
               "CI_DEBUG_SERVICES, a variable read in a rule and one in a comment are not definitions: %r %s" % (found, said))
        for text, names in (('variables: {"CI_PIPELINE_SOURCE": "push"}\n', ["CI_PIPELINE_SOURCE"]),
                            ("variables:\n  GITLAB_FEATURES: x\n", ["GITLAB_FEATURES"]),
                            ("  - CI_X: y\n", ["CI_X"]), ("variables: {A: b, CI_Y: z}\n", ["CI_Y"]),
                            ("  script: 'echo CI_X: y'\n", []), ("x: \"$CI_COMMIT_SHA: y\"\n", [])):
            found = [n for _, n in ci_variable_definitions({"a.yml": text})]
            expect(found == names, "%r defines %s: %s" % (text, names or "nothing", found))
        shallow = os.path.join(work, "shallow")
        subprocess.run(["git"] + quiet_git + ["clone", "-q", "--depth", "1",
                                              "file:///" + origin.replace(os.sep, "/").lstrip("/"), shallow],
                       check=True, capture_output=True)
        expect("shallow" in (change_base(shallow, environ)[2] or ""), "a shallow clone is refused: %r"
               % (change_base(shallow, environ),))
        g(clone, "remote", "set-url", "origin", os.path.join(work, "nowhere"))
        expect("could not be fetched" in (change_base(clone, environ)[2] or ""), "a master that cannot be fetched is refused")
        orphan = os.path.join(work, "orphan")
        os.makedirs(orphan)
        subprocess.run(["git"] + quiet_git + ["init", "-q", orphan], check=True, capture_output=True)
        commit(orphan, "x.txt", "unrelated\n")
        g(orphan, "remote", "add", "origin", origin)
        expect("no merge base" in (change_base(orphan, environ)[2] or ""), "a history unrelated to master is refused")

        # The base's copy of the gates: run over HEAD when the change alters their code.
        copies = os.path.join(work, "copies")
        os.makedirs(copies)
        subprocess.run(["git"] + quiet_git + ["init", "-q", copies], check=True, capture_output=True)
        stub = ("import os, sys\nprint('copy %s', sys.argv[1:], os.path.isfile(os.path.join('src', 'a.cpp')))\n"
                "sys.exit(%d)\n")
        commit(copies, "src/a.cpp", "int main() { return 0; }\n")
        gate_base = commit(copies, "build/ci/security-gate.py", stub % ("base", 1))
        commit(copies, "src/a.cpp", "int main() { return 1; }\n")
        said = []
        expect(base_copy_verdict(copies, gate_base, "build/ci/security-gate.py", ["--x"], said.append, {}) is None,
               "a change that leaves the gates' code alone runs no base's copy: %s" % said)
        commit(copies, "build/ci/security-gate.py", stub % ("head", 0))
        said = []
        code = base_copy_verdict(copies, gate_base, "build/ci/security-gate.py", ["--x"], said.append, {})
        expect(code == 1 and any("copy base ['--x'] True" in s for s in said),
               "a change to the gates' code is judged by the base's copy too, over HEAD's tree: %r %s" % (code, said))
        expect(any(s == "The base's copy of build/ci/security-gate.py exited 1." for s in said),
               "the gate says how the base's copy ended, as build/land-via-mr.py reads it: %s" % said)
        # No variable stops it, the one that once did among them: whether it runs is read
        # from the code that runs.
        said = []
        code = base_copy_verdict(copies, gate_base, "build/ci/security-gate.py", [], said.append,
                                 {"HM_GATE_BASE_COPY": "1", "HM_GATE_BASE_COPY_SKIP": "true"})
        expect(code == 1, "a variable does not stop the base's copy: %r %s" % (code, said))
        # The base's copy itself, in its worktree, runs over files of GATE_CODE that are
        # the base's: none differs, so it runs no copy of its own - whatever their line
        # endings.
        gate_path = os.path.join(copies, "build", "ci", "security-gate.py")
        for ending in ("\n", "\r\n"):
            with open(gate_path, "wb") as f:
                f.write((stub % ("base", 1)).replace("\n", ending).encode("utf-8"))
            expect(base_copy_verdict(copies, gate_base, "build/ci/security-gate.py", [], said.append, {}) is None,
                   "a checkout whose gates' code is the base's (%r endings), as the base's copy's is, runs no copy of "
                   "its own" % ending)
        g(copies, "checkout", "--", "build/ci/security-gate.py")
        expect(running_gate_code(copies, gate_base) == (["build/ci/security-gate.py"], None),
               "the gates' code that runs is told from the base's: %r" % (running_gate_code(copies, gate_base),))
        # A worktree made for a base's copy that still finds the gates' code not the
        # base's stops there, unable to check, and runs no copy of its own.
        bounded = os.path.join(work, BASE_COPY_DIR + "bound", "tree")
        g(copies, "worktree", "add", "--detach", "--quiet", bounded, "HEAD")
        said = []
        code = base_copy_verdict(bounded, gate_base, "build/ci/security-gate.py", [], said.append, {})
        expect(code == 2 and any("runs as the base's copy" in s for s in said),
               "a base's copy's worktree whose gates' code is not the base's runs no copy: %r %s" % (code, said))
        g(copies, "worktree", "remove", "--force", bounded)
        expect(len(g(copies, "worktree", "list").splitlines()) == 1, "the base's copy's worktree is taken away after it")
        expect(base_copy_verdict(copies, gate_base, "build/ci/mr-dependency-check.py", [], said.append, {}) is None,
               "a gate the base has no copy of is judged by the change's alone")
        # And end to end: a base whose gate asks this module's base_copy_verdict, in the
        # worktree it is run in, whether to run a copy of its own - which ends there.
        nested = ("import importlib.util, os, sys\n"
                  "spec = importlib.util.spec_from_file_location('gate_under_test', %r)\n"
                  "gate = importlib.util.module_from_spec(spec)\n"
                  "spec.loader.exec_module(gate)\n"
                  "root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))\n"
                  "print('nested copy', gate.base_copy_verdict(root, sys.argv[1], 'build/ci/security-gate.py', [sys.argv[1]], "
                  "lambda line: None))\n" % os.path.abspath(__file__))
        nested_base = commit(copies, "build/ci/security-gate.py", nested)
        commit(copies, "build/ci/security-gate.py", stub % ("head", 0))
        said = []
        code = base_copy_verdict(copies, nested_base, "build/ci/security-gate.py", [nested_base], said.append,
                                 dict(os.environ))
        expect(code == 0 and any("nested copy None" in s for s in said),
               "the base's copy, run in its worktree, runs none of its own: %r %s" % (code, said))
        expect(len(g(copies, "worktree", "list").splitlines()) == 1, "and every worktree is taken away after it")
        # A base whose blob has CRLF endings is the same code as its LF checkout.
        crlf_base = commit(copies, "build/ci/security-gate.py", (stub % ("base", 1)).replace("\n", "\r\n"))
        commit(copies, "build/ci/security-gate.py", stub % ("head", 0))
        with open(gate_path, "wb") as f:
            f.write((stub % ("base", 1)).encode("utf-8"))
        expect(running_gate_code(copies, crlf_base) == ([], None),
               "a base's blob with CRLF endings is the code a checkout with LF holds: %r"
               % (running_gate_code(copies, crlf_base),))
        g(copies, "checkout", "--", "build/ci/security-gate.py")
        for own_code, copy_code, verdict in ((0, None, 0), (0, 0, 0), (0, 1, 1), (1, 0, 1), (0, 2, 2), (2, 1, 1), (2, 0, 2)):
            expect(combined(own_code, copy_code) == verdict, "the change's copy %s and the base's %s give %s" % (
                own_code, copy_code, verdict))
    except (OSError, subprocess.CalledProcessError) as e:
        failures.append("change_base's repositories could not be made: %s %s" % (e, getattr(e, "stderr", "") or ""))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return failures


def change_gate_failures():
    """The self-test's cases for what a change may not add (change_problems), for what it
    may not narrow or put out of the scan's reach, for what the scan gave up on, and for
    the change's own exceptions in whole-tree mode (merge_request_evaluate), against a
    repository made here: offline."""
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    # The markers, in pieces, as SUPPRESSION_MARKERS spells them. Semgrep takes any word
    # that begins with its short form after a blank for its marker ("# " then the short
    # form and "aphore" silences the line, as Semgrep 1.174 was seen to on 6 October
    # 2026): so is it counted.
    sem, lint, suppress = "no" + "sem" + "grep", "NO" + "LINT", "cppcheck-" + "suppress"
    for text, count in (("x = 1  # %s\n" % sem, 1), ("x = 1  # %s: python.rule\n" % sem.upper(), 1),
                        ("x = 1  # %s\n" % sem[:5], 1), ("// %s\n// %s\n" % (sem, sem), 2),
                        ("x = 1  # %saphore\n" % sem[:5], 1),
                        ("int a; // %s\n" % lint, 1), ("int a; // %s(bugprone-x)\n" % lint, 1),
                        ("// %sNEXTLINE\n" % lint, 1), ("// %sBEGIN\n// %sEND\n" % (lint, lint), 1),
                        ("// %s\n" % lint.lower(), 0), ("// %s nullPointer\n" % suppress, 1),
                        ("// %s-begin nullPointer\n// %s-end\n" % (suppress, suppress), 1),
                        ("// %s-file nullPointer\n" % suppress, 1), ("// %s[a,b]\n" % suppress, 1),
                        ("--suppressions-list=build/ci/%sions.txt\n" % suppress, 0),
                        ("Při pře%s těla\n" % sem[:5], 0)):
        expect(suppression_markers(text.encode("utf-8")) == count, "%r holds %d suppression marker(s) as the tools read it; "
               "the count is %d" % (text, count, suppression_markers(text.encode("utf-8"))))
    expect(suppression_markers(b"\0\1 // " + sem.encode("ascii")) == 0, "a binary file holds no suppression marker")
    expect(suppression_markers(b"\0\1 // " + sem.encode("ascii"), "src/a.cpp") == 1,
           "a source file with a NUL byte in it, which the compiler and the scans still read, has its markers counted")
    expect(suppression_markers(("// %s\n" % sem).encode("utf-16")) == 1, "a UTF-16 file's markers are counted")
    for path, counted in (("src/a.cpp", True), ("README.md", False), ("hmailserver/docs/ContinuousIntegration.md", False),
                          ("build/ci/semgrep-baseline.txt", False), ("build/ci/static-analysis-baseline.txt", False),
                          (EXCEPTIONS, False), (GENERATED_DOCUMENTS[0], False), ("build/ci/cppcheck-suppressions.txt", True),
                          ("hmailserver/source/Server/Common/Util/ProfilesData.cpp", True), ("build/ci/security-gate.py", True)):
        expect(markers_counted(path) == counted, "%s's markers are %scounted" % (path, "" if counted else "not "))
    for wrong in (b":100644 100644 a b\0x\0", b":100644 100644 a b R100\0old\0"):
        try:
            _raw_diff(wrong)
            failures.append("git diff --raw output that cannot be read is refused: %r" % wrong)
        except ValueError:
            pass
    # A generated document is left out only while every scan leaves it out.
    for path in GENERATED_DOCUMENTS:
        expect(findings_module.GENERATED.search(path), "%s is one the source scans leave out (security-findings.py's "
                                                       "GENERATED)" % path)
    try:
        with open(os.path.join(ROOT, ".gitlab", "ci", "gitlab-security.yml"), encoding="utf-8") as f:
            security_text = f.read()
    except OSError:
        security_text = ""
    tree_variables = scope_variables({SAST_JOB_FILE: security_text})
    excluded, readable = exclusions(tree_variables)
    for path in GENERATED_DOCUMENTS:
        expect(path in excluded, "%s is in semgrep-sast's SAST_EXCLUDED_PATHS (.gitlab/ci/gitlab-security.yml), or its "
                                 "markers would count" % path)
    # Each of the tree's exclusions is held to the root: one with no slash would leave out
    # a directory of that name at any depth.
    expect(readable and excluded and all(_pattern_parts(e)[0] for e in excluded),
           "every entry of semgrep-sast's SAST_EXCLUDED_PATHS is held to the root (a slash before its last character): %s"
           % [e for e in excluded if not _pattern_parts(e)[0]])
    expect(tree_variables.get("SAST_SEMGREP_EXCLUDED_PATHS") == [""],
           "semgrep-sast sets SAST_SEMGREP_EXCLUDED_PATHS to nothing itself, so that no CI file's value reaches it: %s"
           % tree_variables.get("SAST_SEMGREP_EXCLUDED_PATHS"))
    expect(analyzer_image(security_text) and analyzer_image(security_text).startswith(
        "registry.gitlab.com/hmailserver/hmailserver/mirror/semgrep:"), "semgrep-sast's image is read from its job: %s"
           % analyzer_image(security_text))
    expect(BASELINES == findings_module.BASELINES, "the baselines this gate holds are the ones the source scans read: %s, %s"
           % (BASELINES, findings_module.BASELINES))
    # The exclusions, as Semgrep's --exclude and GitLab's pathfilter read them (seen on 6
    # October 2026 with the analyzer's Semgrep 1.174: tests leaves out a/tests/t.py,
    # /tests only tests/t.py, and the template's build/ any build directory).
    for pattern, path, out in (("tests", "a/b/tests/c.py", True), ("tests", "tests/c.py", True), ("/tests", "a/tests/c.py", False),
                               ("/tests", "tests/c.py", True), ("hmailserver/test", "hmailserver/test/x.cs", True),
                               ("hmailserver/test", "x/hmailserver/test/x.cs", False), ("build/", "a/build/x.py", True),
                               ("build/", "build", False), ("*.min.js", "a/b.min.js", True), ("src/*.cpp", "src/a.cpp", True),
                               ("src/*.cpp", "src/b/a.cpp", False), ("a/**/c", "a/b/c/d.py", True), ("", "x", False)):
        expect(excluded_by(pattern, path) == out, "%r %s %s" % (pattern, "leaves out" if out else "does not leave out", path))
    for entry, by, out in (("/tests", "tests", True), ("a/b/tests/c", "tests", True), ("a/tests", "/tests", False),
                           ("tests", "/tests", False), ("src/x", "src", True), ("src", "src/x", False), ("src/x", "/src", True),
                           ("a/*.py", "a", False), ("a/b.py", "a/*.py", True), ("x", "x", True), ("lib", "libraries", False)):
        expect(subsumed(entry, by) == out, "%r is %scovered by %r" % (entry, "" if out else "not ", by))
    for text, wanted in (('  SAST_EXCLUDED_PATHS: "a, b"\n', {"SAST_EXCLUDED_PATHS": ["a, b"]}),
                         ("  SAST_EXCLUDED_PATHS: a, b\n", {"SAST_EXCLUDED_PATHS": ["a, b"]}),
                         ("variables: {SAST_X: 'it''s', B: c}\n", {"SAST_X": ["it's"]}),
                         ("variables: {SAST_X: plain, B: c}\n", {"SAST_X": ["plain"]}),
                         ("  SAST_X: |\n    a\n", {"SAST_X": [None]}), ("  SAST_X: *anchor\n", {"SAST_X": [None]}),
                         ("  # SAST_X: a\n  - if: $SAST_DISABLED == 'true'\n", {}), ('  "SEMGREP_Y": ""\n', {"SEMGREP_Y": [""]}),
                         ("  HM_TIDY_CHECKS: '-*'\n", {"HM_TIDY_CHECKS": ["-*"]}),
                         ("  script: 'echo SAST_X: y'\n", {}), ("  - SAST_X: y\n", {"SAST_X": ["y"]})):
        expect(scope_variables({"a.yml": text}) == wanted, "%r defines %s: %s" % (text, wanted, scope_variables({"a.yml": text})))

    # A list in .gitignore's syntax leaves out more when it gains a line, loses a "!"
    # line, or puts its lines in another order while it has one; a "!" line gained, or a
    # line lost, leaves out less. cppcheck's list has no "!" lines.
    ignore = "hmailserver/test/\n!hmailserver/test/keep.py\nlibraries/\n"
    for path, head, narrows in (
            ("build/ci/semgrepignore", ignore.replace("!hmailserver/test/keep.py\n", ""), "loses 1 line(s)"),
            ("build/ci/semgrepignore", ignore + "!libraries/kept.py\n", None),
            ("build/ci/semgrepignore", ignore.replace("libraries/\n", ""), None),
            ("build/ci/semgrepignore", "!hmailserver/test/keep.py\nhmailserver/test/\nlibraries/\n", "another order"),
            ("build/ci/semgrepignore", "# a comment moved\n" + ignore, None),
            ("build/ci/semgrepignore", ignore + "src/\n", "gains 1 line(s)"),
            ("build/ci/semgrepignore", ignore.replace("!hmailserver/test/keep.py", "\\!hmailserver/test/keep.py"),
             "gains 1 line(s)"),
            ("build/ci/semgrepignore", None, None),
            ("src/.semgrepignore", ignore + "!b.py\n", None),
            ("src/.semgrepignore", "!b.py\n", "loses 1 line(s)"),
            ("build/ci/cppcheck-suppressions.txt", "!x\n", "gains 1 line(s)")):
        found = list_narrowings(path, ignore, head)
        expect(any(narrows in f for f in found) if narrows else found == [],
               "%s as %r from %r narrows %s: %s" % (path, head, ignore, narrows or "nothing", found))
    # The scan scripts: the lines between the marks, and the tools' options outside them.
    marked = ("#!/bin/bash\necho start\n# scan-scope begin: what Semgrep reads\nsemgrep scan --config r \\\n"
              "  --include '*.py' --include '*.cs' \\\n  .\n# scan-scope end\necho done\n")
    inside, stray, parts, problem = scan_scope(marked)
    expect(parts == 1 and problem is None and stray == [] and inside == [
        "semgrep scan --config r \\", "--include '*.py' --include '*.cs' \\", "."],
           "scan_scope reads the marked lines: %r" % ((inside, stray, parts, problem),))
    for what, head, narrows in (
            ("an option added between the marks", marked.replace("'*.cs'", "'*.cs' --exclude src"), "changes the lines"),
            ("an include taken away", marked.replace(" --include '*.cs'", ""), "loses \"--include"),
            ("the marked lines in another order", marked.replace("  .\n", "").replace("semgrep scan", "  .\nsemgrep scan"),
             "another order"),
            ("an option put outside the marks", marked.replace("echo done\n", "semgrep scan --exclude src .\necho done\n"),
             "an option outside the lines marked"),
            ("a line without an option outside the marks", marked.replace("echo done", "echo finished"), None),
            ("a comment between the marks", marked.replace("  .\n", "  # the tree\n  .\n"), None),
            ("the marks taken away", marked.replace("# scan-scope begin: what Semgrep reads\n", "").replace(
                "# scan-scope end\n", ""), "no longer marks"),
            ("a mark that does not pair", marked.replace("# scan-scope end\n", ""), "the last part has no end"),
            ("an end with no part", marked.replace("echo done\n", "echo done\n# scan-scope end\n"), "ends no part"),
            ("a part begun inside another", marked.replace("  .\n", "# scan-scope begin: again\n  .\n"),
             "begins inside another part"),
            ("the script taken away", None, None)):
        found = script_narrowings("build/ci/semgrep.sh", marked, head)
        expect(any(narrows in f for f in found) if narrows else found == [],
               "%s narrows %s: %s" % (what, narrows or "nothing", found))
    unmarked = marked.replace("# scan-scope begin: what Semgrep reads\n", "").replace("# scan-scope end\n", "")
    expect(script_narrowings("build/ci/semgrep.sh", unmarked, marked) == [],
           "marks made around the lines the base has narrow nothing: %s" % script_narrowings("build/ci/semgrep.sh", unmarked,
                                                                                              marked))
    expect(any("which its base does not have" in f for f in script_narrowings(
        "build/ci/semgrep.sh", unmarked, marked.replace("'*.cs'", "'*.cs' --exclude src"))),
           "marks made around a line the base does not have narrow")
    expect(scope_only("build/ci/semgrep.sh", marked, marked.replace("'*.cs'", "'*.cs' --exclude src")) and
           not scope_only("build/ci/semgrep.sh", marked, marked.replace("echo done", "echo finished")) and
           not scope_only("build/ci/semgrep.sh", unmarked, marked),
           "a scan script changed between its marks alone is the narrowing's own file, not one changed besides")
    # The tree's scan scripts mark what their tools read, with no option outside it, and
    # static-analysis.sh takes no check list from a job's variables.
    for path, tool in (("build/ci/static-analysis.sh", "cppcheck --project="), ("build/ci/static-analysis.sh", "clang-tidy -p"),
                       ("build/ci/static-analysis.sh", "--config-file=build/ci/clang-tidy.yaml"),
                       ("build/ci/static-analysis.sh", "CPPCHECK FAILED"),
                       ("build/ci/static-analysis.sh", 'cat "$build_dir"/tidy/*.txt > "$out/clang-tidy.txt"'),
                       ("build/ci/static-analysis.sh", 'tidy_one "$tidy_file" "${config[@]}"'),
                       ("build/ci/semgrep.sh", "\"$semgrep\" scan"), ("build/ci/semgrep.sh", "packs="),
                       ("build/ci/semgrep.sh", "for pack in $packs; do"),
                       ("build/ci/semgrep.sh", "https://semgrep.dev/c/$pack")):
        try:
            with open(os.path.join(ROOT, *path.split("/")), encoding="utf-8") as f:
                script = f.read()
        except OSError as e:
            script = ""
            failures.append("%s cannot be read: %s" % (path, e))
        inside, stray, parts, problem = scan_scope(script)
        expect(parts and problem is None and not stray, "%s marks what its tools read, with no option outside the marks: "
                                                        "%r" % (path, (parts, problem, stray)))
        expect(any(tool in line for line in inside), "%s runs %s between its marks" % (path, tool))
        if path.endswith("static-analysis.sh"):
            expect(re.search(r'if \[ -n "\$\{HM_TIDY_CHECKS:-\}" \] && \[ "\$\{GITLAB_CI:-\}" = "true" \]; then\n.*\n\s*exit 2',
                             script), "static-analysis.sh refuses HM_TIDY_CHECKS in a GitLab job")
        else:
            expect('if [ ! -f build/ci/semgrepignore ]; then' in script, "semgrep.sh refuses to run without its ignore list")

    # Semgrep's notifications, word for word as SARIF_READ_FOR's analyzer wrote them: a
    # rule's timeout holds the file it names, a parse that stumbled is counted; a kind in
    # neither list, a timeout naming no file, or no run at all cannot be read.
    sarif_dir = tempfile.mkdtemp(prefix="security-gate-sarif-")
    try:
        def holes(notes, paths, runs=True):
            with open(os.path.join(sarif_dir, SEMGREP_SARIF), "w", encoding="utf-8") as f:
                json.dump({"version": "2.1.0", "runs": [{"tool": {"driver": {"name": "Semgrep OSS", "semanticVersion": "1.174.0"}},
                                                         "invocations": [{"executionSuccessful": True,
                                                                          "toolExecutionNotifications": list(notes)}],
                                                         "results": []}] if runs else []}, f)
            return sarif_holes(sarif_dir, paths)

        fixture = holes(SARIF_FIXTURE, ["src/deep/big.py", "src/small.py", "hmailserver/source/Server/COM/COMAuthentication.cpp",
                                        "hmailserver/source/Server/Common/Util/RestApiSettings.cpp"])
        expect(fixture[2:] == (None, None) and len(fixture[0]) == 1 and fixture[0][0].startswith("src/deep/big.py: ")
               and fixture[1] == ["hmailserver/source/Server/COM/COMAuthentication.cpp",
                                  "hmailserver/source/Server/Common/Util/RestApiSettings.cpp"],
               "the analyzer's own notifications are read: a timeout held, two stumbles counted: %r" % (fixture,))
        expect(holes(SARIF_FIXTURE, ["src/small.py"])[:2] == ([], []), "a file no notification names is not held")
        renamed = [dict(SARIF_FIXTURE[0], descriptor={"id": "Rule timeout"})]
        expect("of a kind this does not know ('Rule timeout'" in (holes(renamed, ["src/deep/big.py"])[3] or ""),
               "a notification of a kind in neither list cannot be read: %r" % (holes(renamed, ["src/deep/big.py"]),))
        unnamed = [dict(SARIF_FIXTURE[0], message={"text": "Timeout when running tmp.slow-pattern:\n "})]
        expect("names no file" in (holes(unnamed, ["src/deep/big.py"])[3] or ""),
               "a timeout whose text names no file cannot be read: %r" % (holes(unnamed, []),))
        located = [dict(unnamed[0], locations=[{"physicalLocation": {"artifactLocation": {"uri": "src/deep/big.py"}}}])]
        expect(len(holes(located, ["src/deep/big.py"])[0] or []) == 1,
               "a timeout that names its file by its location is held: %r" % (holes(located, ["src/deep/big.py"]),))
        expect("no run with its invocations" in (holes([], [], runs=False)[3] or ""), "a SARIF with no run cannot be read")
        expect(holes([], ["src/a.cpp"]) == ([], [], None, None), "a SARIF with no notification holds nothing")
    finally:
        shutil.rmtree(sarif_dir, ignore_errors=True)
    # The wording is the pinned analyzer's: a re-pin reads it again (mirror-semgrep.sh).
    pinned = re.search(r"/mirror/semgrep:([0-9][0-9.]*)-[0-9a-f]{12}@", analyzer_image(security_text) or "")
    expect(pinned and pinned.group(1) == SARIF_READ_FOR,
           "semgrep-sast pins analyzer %s, and sarif_holes read Semgrep's notifications as %s writes them: read the new "
           "one's (build/ci/mirror-semgrep.sh's header), then set SARIF_READ_FOR and SARIF_FIXTURE"
           % (pinned.group(1) if pinned else "(none found)", SARIF_READ_FOR))

    today = datetime.date(2026, 10, 1)
    work = tempfile.mkdtemp(prefix="security-gate-change-")
    repo, artifacts = os.path.join(work, "repo"), os.path.join(work, "artifacts")

    def g(*arguments):
        return subprocess.run(["git", "-C", repo] + list(arguments), check=True, capture_output=True,
                              text=True).stdout.strip()

    def exception(finding, expires="2027-03-01"):
        return ("exceptions:\n  - scan: semgrep-sast\n    finding: %s\n    what: 'the thing'\n"
                "    reason: 'Read in the code: the input is a fixed literal, never a peer''s.'\n"
                "    reviewed-by: Progressiverobot\n    reviewed: 2026-09-26\n    expires: %s\n" % (finding, expires))

    def change(start, files, renames=(), message="a change"):
        g("reset", "-q", "--hard", start)
        for old, new in renames:
            os.makedirs(os.path.dirname(os.path.join(repo, *new.split("/"))), exist_ok=True)
            g("mv", old, new)
        for relative, content in files.items():
            path = os.path.join(repo, *relative.split("/"))
            if content is None:
                os.remove(path)
                continue
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(content if isinstance(content, bytes) else content.encode("utf-8"))
        g("add", "-A")
        g("commit", "-q", "--no-verify", "-m", message)
        return g("rev-parse", "HEAD")

    def verdict(base, severity="Medium", notes=(), analyzer="semgrep", sarif=True):
        """(exit, what it said), the report holding one strcpy at src/a.cpp:2 of severity,
        and semgrep.sarif the notifications notes ([(kind, text)])."""
        if os.path.isdir(artifacts):
            shutil.rmtree(artifacts)
        os.makedirs(artifacts)
        with open(os.path.join(artifacts, GITLAB_SAST_REPORT), "w", encoding="utf-8") as f:
            json.dump({"version": "15.0.4", "scan": {"status": "success", "type": "sast", "analyzer": {"id": analyzer},
                                                     "scanner": {"id": "semgrep"}, "start_time": "2026-10-01T10:00:00"},
                       "vulnerabilities": [{"id": "random", "name": "strcpy", "severity": severity, "description": "strcpy",
                                            "identifiers": [{"type": "semgrep_id", "name": "c.strcpy", "value": "c.strcpy"}],
                                            "location": {"file": "src/a.cpp", "start_line": 2}}]}, f)
        if sarif:
            with open(os.path.join(artifacts, SEMGREP_SARIF), "w", encoding="utf-8") as f:
                json.dump(sarif if isinstance(sarif, dict) else {"version": "2.1.0", "runs": [{"results": [], "invocations": [
                    {"toolExecutionNotifications": [{"descriptor": {"id": kind}, "level": "warning", "message": {"text": text}}
                                                    for kind, text in notes]}]}]}, f)
        said = []
        held = findings_module.ROOT
        findings_module.ROOT = repo
        findings_module._lines_cache.clear()
        findings_module._top_level = None
        try:
            return merge_request_main(repo, artifacts, base, None, today, say=said.append,
                                      not_before=datetime.datetime(2026, 10, 1, 9, 59)), "\n".join(said)
        finally:
            findings_module.ROOT = held
            findings_module._lines_cache.clear()
            findings_module._top_level = None

    def case(what, start, files, expected, needle=None, renames=(), severity="Medium", unsaid=(), **report):
        change(start, files, renames, what)
        code, said = verdict(start, severity, **report)
        if code != expected or (needle and needle not in said) or any(word in said for word in unsaid):
            failures.append("%s: exit %d, not %d%s%s\n%s" % (
                what, code, expected, (" saying %r" % needle) if needle else "",
                (" and never %s" % ", ".join(unsaid)) if unsaid else "", said))

    try:
        os.makedirs(repo)
        subprocess.run(["git", "init", "-q", repo], check=True, capture_output=True)
        for key, value in (("user.name", "self-test"), ("user.email", "self-test@example.invalid"),
                           ("commit.gpgsign", "false"), ("core.autocrlf", "false")):
            g("config", key, value)
        a, b, c = "a" * 20, "b" * 20, "c" * 20
        baseline = "# the semgrep job's baseline\n%s\tsemgrep\trule.one\tsrc/b.py:2\tm\tthe reason its triage gave\n" \
                   "%s\tsemgrep\trule.two\tsrc/b.py:3\tm\n" % (a, b)
        source = "int main() {\n  char b[4]; strcpy(b, argv[1]);\n  return atoi(x);\n}\n"
        python = "import os\nos.chmod(p, 0o755)  # %s\nprint(1)\n" % sem
        library = "".join("int f%d(int x) { return x * %d; }\n" % (n, n) for n in range(12)) + "int g(); // %s\n" % lint

        def security(image="registry.example/semgrep:1@sha256:%s" % ("1" * 64), excluded="tests, src/test", more=""):
            return ("semgrep-sast:\n  image:\n    name: %s\n  variables:\n    SAST_EXCLUDED_PATHS: \"%s\"\n"
                    "    SAST_SCANNER_ALLOWED_CLI_OPTS: \"--timeout=120\"\n%s" % (image, excluded, more))
        # The first commit has no parent to reset to: made by hand.
        for relative, content in {"src/a.cpp": source, "src/b.py": python, "src/c.cpp": library,
                                  "build/ci/semgrep-baseline.txt": baseline, EXCEPTIONS: "exceptions: []\n",
                                  SAST_JOB_FILE: security(), ".gitlab-ci.yml": "stages: [test]\n",
                                  "build/ci/cppcheck-suppressions.txt": "missingInclude\n",
                                  "build/ci/semgrepignore": ignore, "build/ci/semgrep.sh": marked,
                                  "docs/Guide.md": "Markers: %s\n" % sem}.items():
            path = os.path.join(repo, *relative.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(content)
        g("add", "-A")
        g("commit", "-q", "--no-verify", "-m", "base")
        base = g("rev-parse", "HEAD")
        semgrep_baseline = "build/ci/semgrep-baseline.txt"
        other_baseline = BASELINES["static-analysis"].replace(os.sep, "/")

        expect(verdict(base)[0] == 0, "a change that changes nothing passes: %s" % (verdict(base),))
        # The baselines only shrink.
        case("an entry added to a baseline", base, {semgrep_baseline: baseline + "%s\tsemgrep\trule.three\tsrc/b.py:1\tm\n" % c},
             1, "1 entry the base's copy does not have, %s" % c)
        case("an entry dropped from a baseline", base, {semgrep_baseline: baseline.replace("%s\t" % b, "# gone ")}, 0)
        case("a reason taken away", base, {semgrep_baseline: baseline.replace("\tthe reason its triage gave", "")}, 1,
             "whose reason this change took away, %s" % a)
        case("a reason left empty", base, {semgrep_baseline: baseline.replace("the reason its triage gave", "  ")}, 1,
             "whose reason this change took away")
        case("a reason written again", base, {semgrep_baseline: baseline.replace("its triage gave", "its triage gave, read again")},
             0)
        case("a reason given", base, {semgrep_baseline: baseline.replace("rule.two\tsrc/b.py:3\tm", "rule.two\tsrc/b.py:4\tm\tnow read")},
             0)
        case("a baseline the base has not", base, {other_baseline: "# new\n%s\tcppcheck\tnullPointer\tsrc/a.cpp:2\tm\n" % c}, 1,
             "%s: 1 entry the base's copy does not have" % other_baseline)
        case("a baseline removed", base, {semgrep_baseline: None}, 0)
        # No suppression marker added.
        case("a marker added", base, {"src/a.cpp": source.replace("argv[1]);", "argv[1]); // %s" % sem)}, 1,
             "src/a.cpp: 1 comment(s) by which a scan skips a finding, 0 at the base")
        case("a clang-tidy marker for the next line added", base, {"src/a.cpp": "// %sNEXTLINE\n" % lint + source}, 1, "src/a.cpp")
        case("a cppcheck block suppression added", base, {"src/a.cpp": "// %s-begin x\n" % suppress + source}, 1, "src/a.cpp")
        case("a block's end alone", base, {"src/a.cpp": source + "// %sEND\n" % lint}, 0)
        case("a marked line edited", base, {"src/b.py": python.replace("0o755", "0o750")}, 0)
        case("a marker moved within its file", base,
             {"src/b.py": python.replace("  # %s" % sem, "").replace("print(1)", "print(1)  # %s" % sem)}, 0)
        case("a new file with a marker", base, {"src/new.js": "eval(x); // %s\n" % sem}, 1, "src/new.js: 1 comment(s)")
        case("a file renamed with its marker", base, {}, 0, renames=(("src/c.cpp", "src/d.cpp"),))
        case("a file renamed with a marker more", base, {"src/d.cpp": library + "int h(); // %s\n" % lint}, 1,
             "src/d.cpp: 2 comment(s) by which a scan skips a finding, 1 at the base", renames=(("src/c.cpp", "src/d.cpp"),))
        case("a document renamed into code", base, {}, 1, "src/guide.py: 1 comment(s)", renames=(("docs/Guide.md", "src/guide.py"),))
        case("a document naming the markers", base, {"docs/Guide.md": "Markers: %s, %s, %s\n" % (sem, lint, suppress)}, 0)
        case("a generated document naming them", base, {GENERATED_DOCUMENTS[0]: '"%s %s"\n' % (sem, lint)}, 0)
        case("the exceptions file naming them", base, {EXCEPTIONS: "# not a %s\nexceptions: []\n" % sem}, 0)
        case("a binary file", base, {"src/blob.bin": b"\0\1\2 " + sem.encode("ascii")}, 0)
        case("a source file with a NUL byte and a marker", base, {"src/zero.cpp": b"int x;\0 // " + sem.encode("ascii")}, 1,
             "src/zero.cpp: 1 comment(s)")

        # A narrowing of what a scan reads is a declaration: it lands alone.
        edit = {"src/b.py": python + "print(2)\n"}
        case("a .semgrepignore added with a source change", base, dict(edit, **{".semgrepignore": "src/b.py\n"}), 1,
             "narrows what a scan reads (.semgrepignore gains 1 line(s)")
        case("a .semgrepignore added alone, with its document", base, {".semgrepignore": "src/b.py\n", "docs/Guide.md": "x\n"}, 0,
             "the narrowing lands alone")
        case("a .semgrepignore.local added", base, dict(edit, **{".semgrepignore.local": "src/\n"}), 1, ".semgrepignore.local gains")
        case("a .semgrepignore in a directory", base, dict(edit, **{"src/.semgrepignore": "b.py\n"}), 1, "src/.semgrepignore gains")
        case("GitLab's custom ruleset added", base, dict(edit, **{".gitlab/sast-ruleset.toml": "[semgrep]\n"}), 1,
             ".gitlab/sast-ruleset.toml is added")
        case("an exclusion the base's do not cover", base, dict(edit, **{SAST_JOB_FILE: security(excluded="tests, src/test, src/b.py")}),
             1, "gain 'src/b.py', which the base's do not cover")
        case("exclusions held to the root, which the base's cover", base,
             dict(edit, **{SAST_JOB_FILE: security(excluded="/tests, x/tests, /src/test")}), 0, "nothing narrowed")
        case("SAST_SEMGREP_EXCLUDED_PATHS set to nothing", base,
             dict(edit, **{SAST_JOB_FILE: security(more="    SAST_SEMGREP_EXCLUDED_PATHS: \"\"\n")}), 0, "nothing narrowed")
        case("SAST_SEMGREP_EXCLUDED_PATHS set for every job", base,
             dict(edit, **{".gitlab-ci.yml": "stages: [test]\nvariables:\n  SAST_SEMGREP_EXCLUDED_PATHS: \"src\"\n"}), 1,
             "gain 'src'")
        case("an exclusion in a flow mapping", base,
             dict(edit, **{".gitlab-ci.yml": "stages: [test]\nvariables: {SAST_EXCLUDED_PATHS: \"src/b.py\"}\n"}), 1, "gain 'src/b.py'")
        case("an exclusion this cannot read", base,
             dict(edit, **{".gitlab-ci.yml": "variables:\n  SAST_EXCLUDED_PATHS: >\n    src\n"}), 1, "cannot be told")
        case("the analyzer's options changed", base,
             dict(edit, **{SAST_JOB_FILE: security().replace("--timeout=120", "--timeout=1")}), 1,
             "SAST_SCANNER_ALLOWED_CLI_OPTS is '--timeout=1' in the CI files, where the base has '--timeout=120'")
        case("a ruleset reference set", base,
             dict(edit, **{".gitlab-ci.yml": "variables:\n  SAST_RULESET_GIT_REFERENCE: \"x.example/r\"\n"}), 1,
             "SAST_RULESET_GIT_REFERENCE is")
        case("clang-tidy's checks chosen for the job", base, dict(edit, **{".gitlab-ci.yml": "variables:\n  HM_TIDY_CHECKS: \"-*\"\n"}),
             1, "HM_TIDY_CHECKS is")
        case("a cppcheck suppression added", base,
             dict(edit, **{"build/ci/cppcheck-suppressions.txt": "missingInclude\nnullPointer:src/a.cpp\n"}), 1,
             "cppcheck-suppressions.txt gains 1 line(s)")
        case("a cppcheck suppression taken away", base, dict(edit, **{"build/ci/cppcheck-suppressions.txt": "# none\n"}), 0,
             "nothing narrowed")
        # Semgrep's ignore list on linix: a "!" line lost, or put before what it takes
        # back, narrows; one gained, or a line lost, does not.
        semgrepignore = "build/ci/semgrepignore"
        case("a line that takes back what the ignore list left out, lost", base,
             dict(edit, **{semgrepignore: ignore.replace("!hmailserver/test/keep.py\n", "")}), 1, "loses 1 line(s) that take back")
        case("that line lost alone", base, {semgrepignore: ignore.replace("!hmailserver/test/keep.py\n", "")}, 0,
             "the narrowing lands alone")
        case("that line put before what it takes back", base,
             dict(edit, **{semgrepignore: "!hmailserver/test/keep.py\nhmailserver/test/\nlibraries/\n"}), 1, "another order")
        case("a line that takes back more, gained", base, dict(edit, **{semgrepignore: ignore + "!libraries/kept.py\n"}), 0,
             "nothing narrowed")
        # The lines of semgrep.sh that choose what Semgrep reads: a change of them, or an
        # option put outside them, narrows; it lands alone, semgrep.sh being its own file.
        script = "build/ci/semgrep.sh"
        excluding = marked.replace("'*.cs'", "'*.cs' --exclude src")
        case("an exclusion between semgrep.sh's marks, with a source change", base, dict(edit, **{script: excluding}), 1,
             "changes the lines that choose what its tools read")
        case("an exclusion between semgrep.sh's marks, alone", base, {script: excluding}, 0, "the narrowing lands alone")
        case("an exclusion between semgrep.sh's marks and a line outside them", base,
             {script: excluding.replace("echo done", "echo finished")}, 1, "changes 1 file(s) a scan reads besides")
        case("an exclusion outside semgrep.sh's marks", base,
             dict(edit, **{script: marked.replace("echo done\n", "semgrep scan --exclude src .\necho done\n")}), 1,
             "gives its tools an option outside")
        case("a line of semgrep.sh outside its marks, with a source change", base,
             dict(edit, **{script: marked.replace("echo done", "echo finished")}), 0, "nothing narrowed")
        before_marks = change(base, {script: unmarked}, message="semgrep.sh before its marks")
        case("semgrep.sh's marks made around the lines it had, with a source change", before_marks,
             dict(edit, **{script: marked}), 0, "nothing narrowed")
        case("semgrep.sh's marks made around a line it did not have", before_marks, dict(edit, **{script: excluding}), 1,
             "which its base does not have")
        # ... nor a source file put out of semgrep-sast's reach.
        case("a source file moved into a directory the scan leaves out", base, {}, 1,
             "src/test/b.py: moved from src/b.py, which semgrep-sast reads", renames=(("src/b.py", "src/test/b.py"),))
        case("a source file added under a vendor directory", base, {"src/vendor/x.cpp": "int x;\n"}, 1,
             "src/vendor/x.cpp: GitLab's analyzer reads nothing there")
        case("a minified script added", base, {"src/app.min.js": "eval(x)\n"}, 1, "src/app.min.js: GitLab's analyzer")
        case("a script added under build/", base, {"build/x.py": "print(1)\n"}, 0)
        case("a test added where the tests are left out", base, {"src/test/t.py": "print(1)\n"}, 0)
        # A source file deleted while one of its name is added where the scan reads
        # nothing, rewritten past what git pairs as a move, is moved all the same; one of
        # another name is a test like any other.
        rewritten = "".join("def case_%d():\n    return %d\n" % (n, n) for n in range(8))
        case("a source file moved and rewritten into a directory the scan leaves out", base,
             {"src/b.py": None, "src/test/b.py": rewritten}, 1, "while this change deletes src/b.py, of the same name")
        case("a source file deleted, and a test of another name added", base,
             {"src/b.py": None, "src/test/t_b.py": rewritten}, 0)
        case("a source file deleted, and one of its name added where the scan reads", base,
             {"src/b.py": None, "src/lib/b.py": rewritten}, 0)
        # Where an exclusion of the project's, landed first, names the place, the file
        # there is declared unread.
        declared = change(base, {SAST_JOB_FILE: security(excluded="tests, src/test, src/vendor")}, message="declared")
        case("a vendor file where an exclusion of the project's names the place", declared, {"src/vendor/x.cpp": "int x;\n"}, 0)
        # ... nor a file the scan gave up on.
        case("a changed file the scan gave up on", base, {"src/a.cpp": source + "// more\n"}, 1, "gave up on it",
             notes=[("Timeout", "Timeout when running c.strcpy on src/a.cpp:\n ")])
        case("a changed file the parser stumbled in", base, {"src/a.cpp": source + "// more\n"}, 0, "the parser stumbled in 1",
             notes=[("Syntax error", "Syntax error at line src/a.cpp:3:\n `x` was unexpected")])
        case("a file the scan gave up on that the change does not touch", base, {"src/b.py": python + "print(2)\n"}, 0,
             notes=[("Timeout", "Timeout when running c.strcpy on src/a.cpp:\n ")])
        case("no SARIF beside the report", base, {"src/b.py": python + "print(2)\n"}, 1, "semgrep.sarif", sarif=False)
        # A SARIF in words this does not know cannot be read: never a pass.
        case("a notification of a kind this does not know", base, {"src/b.py": python + "print(2)\n"}, 2,
             "of a kind this does not know ('Rule timeout'",
             notes=[("Rule timeout", "Rule timeout when running c.strcpy on src/a.cpp:\n ")])
        case("a timeout that names no file", base, {"src/b.py": python + "print(2)\n"}, 2, "names no file",
             notes=[("Timeout", "Timeout when running c.strcpy\n ")])
        case("a SARIF with no run", base, {"src/b.py": python + "print(2)\n"}, 2, "no run with its invocations",
             sarif={"version": "2.1.0", "runs": []})
        case("a notification this does not know, and a marker added", base,
             {"src/a.cpp": source.replace("argv[1]);", "argv[1]); // %s" % sem)}, 1, "CANNOT CHECK: semgrep.sarif holds",
             notes=[("Rule timeout", "Rule timeout when running c.strcpy on src/a.cpp:\n ")])
        case("the analyzer's own wording", base, {"src/a.cpp": source + "// more\n"}, 1,
             "src/a.cpp: semgrep-sast gave up on it",
             notes=[(SARIF_FIXTURE[0]["descriptor"]["id"], SARIF_FIXTURE[0]["message"]["text"].replace("src/deep/big.py",
                                                                                                       "src/a.cpp"))])
        # A rule that gave up names a file of the checkout as the analyzer names one, from
        # its root: a path in another form would match no changed file, and the hole pass.
        timed_out = SARIF_FIXTURE[0]["message"]["text"]
        for what, named in (("as ./src/a.cpp", "./src/a.cpp"), ("by its absolute path", "/builds/hm/hm/src/a.cpp"),
                            ("where the checkout holds no file", "lib/a.cpp")):
            case("a timeout naming the changed file %s" % what, base, {"src/a.cpp": source + "// more\n"}, 2,
                 "names no file this can find of the checkout", notes=[("Timeout", timed_out.replace("src/deep/big.py", named))])

        def located(uri):
            return {"version": "2.1.0", "runs": [{"results": [], "invocations": [{"toolExecutionNotifications": [
                {"descriptor": {"id": "Timeout"}, "level": "warning", "message": {"text": "Timeout when running c.strcpy:\n "},
                 "locations": [{"physicalLocation": {"artifactLocation": {"uri": uri}}}]}]}]}]}
        case("a timeout naming the changed file by a file: URI alone", base, {"src/a.cpp": source + "// more\n"}, 2,
             "names no file this can find of the checkout", sarif=located("file:///builds/hm/hm/src/a.cpp"))
        case("a timeout naming the changed file by its location from the root", base, {"src/a.cpp": source + "// more\n"},
             1, "src/a.cpp: semgrep-sast gave up on it", sarif=located("src/a.cpp"))
        # ... nor a report that is not this pipeline's, or one the change commits.
        case("a report another tool wrote", base, {"docs/Guide.md": "x\n"}, 1, "not written by GitLab's Semgrep analyzer",
             analyzer="semgrep-local")
        case("a report committed with the change", base, {GITLAB_SAST_REPORT: "{}\n"}, 1, "carries what a scan writes")

        # Whole-tree mode: the change's own exceptions accept outside the files it
        # touches when it re-pins the analyzer, and only then.
        fp = None
        g("reset", "-q", "--hard", base)
        code, said = verdict(base, "Critical")
        found = re.search(r"fingerprint ([0-9a-f]{20})", said)
        expect(code == 0 and found is None, "a Critical finding in a file the change does not touch is not its: %s" % said)
        altered = {SAST_JOB_FILE: security(more="  timeout: 20 minutes\n")}
        repinned = {SAST_JOB_FILE: security(image="registry.example/semgrep:2@sha256:%s" % ("2" * 64))}
        change(base, altered, message="the scan altered")
        code, said = verdict(base, "Critical")
        found = re.search(r"fingerprint ([0-9a-f]{20})", said)
        expect(code == 1 and found is not None, "a change that alters the scan answers for every finding: %s" % said)
        fp = found.group(1) if found else "0" * 20
        case("the analyzer re-pinned, with its own exception for a file it does not touch", base,
             dict(repinned, **{EXCEPTIONS: exception(fp)}), 0, "accepted by the change's own exception until 2027-03-01",
             severity="Critical")
        case("the scan altered but not re-pinned, with its own exception for a file it does not touch", base,
             dict(altered, **{EXCEPTIONS: exception(fp)}), 1, "each accepted by the base's %s alone" % EXCEPTIONS,
             severity="Critical", unsaid=("accepted by the change's own exception",))
        case("the analyzer re-pinned, with its own exception for a file it touches", base,
             dict(repinned, **{EXCEPTIONS: exception(fp), "src/a.cpp": source + "// more\n"}), 1,
             "accepted only by this change's own", severity="Critical")
        case("the analyzer re-pinned, with its own exception lapsed", base,
             dict(repinned, **{EXCEPTIONS: exception(fp, "2026-09-30")}), 1, "fingerprint %s" % fp, severity="Critical")
        # A file of the scan's configuration taken away, or moved, alters the scan as
        # much as an edit of it does.
        case("the scan's configuration removed", base, {SAST_JOB_FILE: None}, 1,
             "this change alters .gitlab/ci/gitlab-security.yml", severity="Critical")
        case("the scan's configuration moved", base, {}, 1, "this change alters .gitlab/ci/gitlab-security.yml",
             renames=((SAST_JOB_FILE, ".gitlab/ci/moved.yml"),), severity="Critical")
        # One entry that would accept it, and one without its expiry: a file not well
        # formed accepts nothing, the good entry in it included.
        case("the analyzer re-pinned, with its own exceptions not well formed", base,
             dict(repinned, **{EXCEPTIONS: exception(fp) + exception("d" * 20).split("\n", 1)[1].replace("    expires: 2027-03-01\n", "")}),
             1, "this change's own %s cannot be read" % EXCEPTIONS, severity="Critical")
        case("its own exception without the scan altered, in a file it touches", base,
             {EXCEPTIONS: exception(fp), "src/a.cpp": source + "// more\n"}, 1, "fingerprint %s" % fp, severity="Critical")
        # Without the scan altered, a finding outside the changed files is the
        # release's: counted in the public log, never described, whatever the
        # change's own copy says of it.
        case("its own exception without the scan altered, in a file it does not touch", base,
             {EXCEPTIONS: exception(fp), "src/other.txt": "x\n"}, 0, "1 Critical or High outside the changed files",
             severity="Critical", unsaid=(fp, "src/a.cpp", "accepted by the change's own"))
        accepted = change(base, {EXCEPTIONS: exception(fp)}, message="the exception, landed first")
        case("the scan altered, and the base's exception for a file it touches", accepted,
             dict(altered, **{"src/a.cpp": source + "// more\n"}), 0, severity="Critical")
        expect(verdict("0" * 40)[0] == 2, "a base the clone does not have cannot be checked")
    except (OSError, subprocess.CalledProcessError) as e:
        failures.append("the change gate's repository could not be made: %s %s" % (e, getattr(e, "stderr", "") or ""))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return failures


def merge_request_main(root, artifacts, base, exceptions_text=None, today=None, say=print, not_before=None):
    """The change's gate, from a checkout of its head: 0 it may land, 1 it may not, 2 its
    inputs cannot be read. exceptions_text, when given, stands for the base's file;
    not_before, when the pipeline was created, for the report's provenance."""
    inputs, problem = merge_request_inputs(root, base)
    if problem:
        say("CANNOT CHECK: " + problem)
        return 2
    problems = committed_reports(root)
    base_image, head_image = inputs["analyzer"]
    problems += merge_request_evaluate(root, artifacts,
                                       exceptions_text if exceptions_text is not None else inputs["exceptions"],
                                       inputs["changed"], today, say, inputs["own_exceptions"],
                                       analyzer_changed=base_image != head_image, not_before=not_before)
    problems += change_problems(inputs, say)
    # What the scan gave up on, in the files it reads that the change adds or alters.
    head_entries, _ = exclusions(inputs["variables"][1])
    read = [new for _, _, new in inputs["moves"] if SAST_SOURCE.search(new) and unread_reason(new, head_entries) is None]
    held, stumbled, problem, unreadable = sarif_holes(artifacts, read, root)
    if problem:
        problems.append(problem)
    elif not unreadable:
        problems.extend(held)
        say("semgrep-sast's notifications: %d of the %d source file(s) this change adds or alters that it reads were given "
            "up on; the parser stumbled in %d, which it reads on past (listed, not held)%s" % (
                len(held), len(read), len(stumbled), (": " + ", ".join(stumbled[:8])) if stumbled else ""))
    for p in problems:
        say("HELD: " + p)
    if unreadable:
        # Never a pass: what the scan gave up on cannot be told.
        say("CANNOT CHECK: " + unreadable)
        if not problems:
            return 2
    if problems:
        say("This change waits on the %d line(s) HELD above: a finding of severity Critical or High in what it changes "
            "is fixed, or accepted in %s by a change of its own, landed first, with the reason it is not exploitable, a "
            "reviewer and an expiry; a baseline only shrinks, no change adds a comment by which a scan skips a finding, "
            "and a narrowing of what a scan reads lands alone; a report that is missing, did not finish or is not this "
            "pipeline's, or a file the scan gave up on, is a semgrep-sast job to run again (SECURITY.md, "
            "'Vulnerability management policy'; docs/ContinuousIntegration.md, 'Change checks')." % (
                len(problems), EXCEPTIONS))
        return 1
    say("No Critical or High finding of semgrep-sast in what this change touches that the base's %s does not accept, "
        "no baseline grown or a reason taken from it, no comment added by which a scan skips a finding, and nothing "
        "narrowed, moved out of the scan or given up on." % EXCEPTIONS)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--artifacts", default=None, help="where the scans' artifacts are (default: the checkout's root)")
    parser.add_argument("--exceptions", default=None, help="the exceptions file (default: %s)" % EXCEPTIONS)
    parser.add_argument("--today", default=None, help="the date to judge expiry by (YYYY-MM-DD; default: today, UTC)")
    parser.add_argument("--merge-request", metavar="BASE", default=None,
                        help="a change's gate: semgrep-sast alone, the exceptions as BASE has them, the files changed from "
                             "BASE, and no baseline grown or suppression comment added; 'auto' finds BASE as change_base "
                             "does")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    today = None
    if args.today:
        today = _date(args.today)
        if not today:
            print("--today is not a date: %s" % args.today)
            return 2
    text = None
    if args.exceptions:
        with open(args.exceptions, encoding="utf-8") as f:
            text = f.read()
    artifacts = os.path.abspath(args.artifacts or ROOT)
    if args.merge_request is not None:
        base = args.merge_request
        if base == "auto":
            base, code = auto_base(ROOT)
            if code:
                return code
        own = merge_request_main(ROOT, artifacts, base, text, today, not_before=pipeline_created())
        if not re.match(r"^[0-9a-f]{7,64}$", base or ""):
            return own
        arguments = ["--merge-request", base, "--artifacts", artifacts] + (["--today", args.today] if args.today else []) + \
            (["--exceptions", os.path.abspath(args.exceptions)] if args.exceptions else [])
        return combined(own, base_copy_verdict(ROOT, base, "build/ci/security-gate.py", arguments))
    problems = committed_reports(ROOT) + evaluate(ROOT, artifacts, today, text, not_before=pipeline_created())
    for problem in problems:
        print("HELD: " + problem)
    if problems:
        print("The release waits: %d problem(s). Fix the finding, or accept it in %s with a reason, a reviewer and "
              "an expiry (SECURITY.md, 'Before a release')." % (len(problems), EXCEPTIONS))
        return 1
    print("Every scan finished, and every finding the policy counts is in a baseline or accepted.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
