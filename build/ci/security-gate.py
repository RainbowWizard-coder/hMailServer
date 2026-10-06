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
CI image, whose packages the Linux packages are built against.

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
every file, when it alters the scan's configuration, where its own copy of the
exceptions may accept a finding in a file it does not touch (merge_request_evaluate
below); and, whatever the scan found, no entry added to a baseline or reason taken
from one, and no suppression comment added to a file a scan reads (change_problems).
--exceptions, with it, stands for the base's file. Exits 0 when the change may land,
1 when it may not, 2 when its inputs cannot be read. BASE "auto" is change_base's:
CI_MERGE_REQUEST_DIFF_BASE_SHA in a merge request's pipeline, and in a batch* branch's
pipeline, which GitLab holds the branch's merge request to, where HEAD leaves master,
fetched by name; a base that cannot be found exits 2.
"""

import argparse
import datetime
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import uuid
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
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


def gitlab_sast_scan(root, artifacts, accepted, scope=None, outside=None, own=None, own_taken=None):
    """semgrep-sast, GitLab's report: every Critical or High finding, fingerprinted as
    security-findings.py fingerprints its own (the rule, the file, the line's text).
    scope, a set of paths, holds only the findings in those files (a merge request's
    changed files); the others are listed in outside, when given, and hold nothing.
    own, (the changed paths, exceptions), is a change's own copy of the exceptions in
    force, which accepts a finding only in a file the change does not touch (whole-tree
    mode, merge_request_evaluate); what it accepts goes to own_taken."""
    relative = GITLAB_SAST_REPORT
    report, error = _read_json(os.path.join(artifacts, relative))
    if error:
        return (["semgrep-sast: %s - the job did not run for this commit, failed before its report, or its artifact "
                 "did not reach this job (.gitlab/ci/gitlab-security.yml keeps it)" % error], [], 0)
    status = _status_problem(report, relative)
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


def evaluate(root, artifacts, today=None, exceptions_text=None, say=print):
    """The problems that hold the release. Nothing when every scan finished and every
    finding the policy counts is in a baseline or accepted by an exception in force."""
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
            results[scan] = gitlab_sast_scan(root, artifacts, accepted)
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
# accept, with two differences:
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
#   fingerprinted), when every finding in the tree is its business. Then a finding in a
#   file the change does not touch may also be accepted by the change's own copy of
#   the exceptions file, and one in a file it touches still by the base's alone: a
#   change that re-pins a vanished analyzer while the target branch holds findings
#   nobody has accepted can carry their exceptions with it, which no change of its own
#   could land first, since the scan that would judge that change is the one gone.
#
# And whatever the scan found, a change may not take the two ways of silencing a
# finding without a declaration on the base (change_problems): it may not add an entry
# to a source scan's baseline or take an entry's reason away, and it may not raise, in
# any file a scan reads, the count of the comments by which a tool skips a finding.

# A change to one of these can change what the scan finds anywhere in the tree.
# .gitlab/sast-ruleset.toml is the file GitLab's analyzers read a custom ruleset
# from (rules disabled, overridden or added): none is committed, and the one that
# is added changes the scan as much as an edit of its job does.
# The files whose change can change what semgrep-sast finds anywhere, or what this gate
# makes of it: a merge request that changes one answers for every finding in the tree.
# .gitlab-ci.yml among them, since a variable it sets for every job (SAST_EXCLUDED_PATHS,
# SAST_EXCLUDED_ANALYZERS, SAST_RULESET_GIT_REFERENCE...) reaches the scan as
# gitlab-security.yml's own do; and mr-checks.yml, which runs this gate.
SAST_CONFIG = (".gitlab-ci.yml", ".gitlab/ci/gitlab-security.yml", ".gitlab/ci/mr-checks.yml", ".gitlab/sast-ruleset.toml",
               "build/ci/security-gate.py", "build/ci/security-findings.py")


def merge_request_evaluate(root, artifacts, exceptions_text, changed, today=None, say=print, own_exceptions_text=None):
    """The problems that hold a change: semgrep-sast's Critical and High findings in the
    changed files (or anywhere, when the scan's own configuration changed) that
    exceptions_text, the base's, does not accept. When the scan's configuration changed,
    own_exceptions_text, the change's own copy, also accepts a finding in a file the
    change does not touch."""
    today = today or datetime.datetime.now(datetime.timezone.utc).date()
    entries, problems = check_exceptions(exceptions_text)
    if problems:
        return ["the base's %s cannot be read, so nothing is accepted: %s" % (EXCEPTIONS, p) for p in problems]
    in_force, problems = _in_force(entries, today, say)
    whole = sorted(set(changed) & set(SAST_CONFIG))
    scope = None if whole else set(changed)
    outside, own, own_taken = [], None, []
    if whole and own_exceptions_text is not None:
        own_entries, wrong = check_exceptions(own_exceptions_text)
        if not wrong:
            # Its lapsed entries are the base's too, or are the change's to drop: said once.
            own_in_force, wrong = _in_force(own_entries, today, lambda line: None)
        if wrong:
            say("this change's own %s cannot be read, so it accepts nothing: %s" % (EXCEPTIONS, "; ".join(wrong)))
        else:
            own = (frozenset(changed), own_in_force.get("semgrep-sast", []))
    found, taken, total = gitlab_sast_scan(root, artifacts, in_force.get("semgrep-sast", []), scope, outside, own, own_taken)
    if whole:
        say("this change alters %s, which can change what the scan finds anywhere: every finding in the tree counts, "
            "and one in a file it does not touch may be accepted by its own %s as well as by the base's" % (
                ", ".join(whole), EXCEPTIONS))
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
    return problems + found


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


def suppression_markers(data):
    """How many suppression markers the file's bytes hold. A file git would call binary
    holds none, unless it is UTF-16 text with its byte order mark."""
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        text = data.decode("utf-16", "replace")
    elif b"\0" in data[:8000]:
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


def marker_counts(root, base):
    """([(path, count at base, count at HEAD)] for each file the change adds or alters and
    a scan reads, None) or (None, the problem). A file renamed from one whose markers
    were not counted starts from none."""
    raw = _git(root, "diff", "--raw", "-z", "-M", "-l0", "--no-abbrev", "--diff-filter=ACMRT", base, "HEAD")
    if raw.returncode != 0:
        return None, "git diff --raw %s HEAD failed: %s" % (base, _said(raw))
    try:
        items = _raw_diff(raw.stdout)
    except ValueError as e:
        return None, str(e)
    wanted = []
    for status, old_mode, new_mode, old_blob, new_blob, old, new in items:
        if new_mode not in REGULAR_FILE or not markers_counted(new):
            continue
        before = old_blob if (status != "A" and old_mode in REGULAR_FILE and markers_counted(old)) else None
        wanted.append((new, before, new_blob))
    blobs, problem = _blobs(root, [b for _, before, after in wanted for b in (before, after) if b])
    if problem:
        return None, problem
    return [(path, suppression_markers(blobs[before]) if before else 0, suppression_markers(blobs[after]))
            for path, before, after in wanted], None


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
    reason, and a file a scan reads with more suppression markers than its base."""
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
    return problems


def merge_request_inputs(root, base):
    """(inputs, None) or (None, the problem): what the change holds from base to HEAD, as
    the gate reads it: {"changed": the paths it adds or alters, "exceptions": the base's
    exceptions file (none accepts nothing), "own_exceptions": HEAD's, or None,
    "baselines": {path: (the base's text, HEAD's), None for no file}, "markers":
    marker_counts}."""
    if not re.match(r"^[0-9a-f]{7,64}$", base or ""):
        return None, ("no base commit (%r): run it with the merge request's CI_MERGE_REQUEST_DIFF_BASE_SHA, or 'auto', "
                      "in a clone that has it (GIT_DEPTH 0)" % base)
    if _git(root, "cat-file", "-e", base + "^{commit}").returncode != 0:
        return None, "the base commit %s is not in this clone: the job needs the history (GIT_DEPTH 0)" % base
    diff = _git(root, "diff", "--name-only", "-z", "--diff-filter=d", base, "HEAD")
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
    inputs["markers"], problem = marker_counts(root, base)
    if problem:
        return None, problem
    return inputs, None


def sample_reports():
    """{artifact path: text}: what the four scans leave when they finished and found
    nothing. For the self-tests here and in build/ci/release-chain.py."""
    def report(kind, scanner):
        return json.dumps({"version": "15.2.5", "scan": {"status": "success", "type": kind, "scanner": {"id": scanner}},
                           "vulnerabilities": []})
    reports = {GITLAB_SAST_REPORT: report("sast", "semgrep")}
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

        def gitlab(severity="High", status="success"):
            return {"version": "15.0.4", "scan": {"status": status, "type": "sast"},
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

        critical = {GITLAB_SAST_REPORT: gitlab("Critical")}
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
        found = gate(critical, ["src/a.cpp"], "foo: bar\n")
        expect(any("cannot be read" in p for p in found), "an exceptions file the gate cannot read accepts nothing: %s" % found)

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

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


# ---------------------------------------------------------------- the change's base
#
# The commit a change is judged against, for this gate (--merge-request auto) and for
# build/ci/mr-dependency-check.py (--base auto): the change is what lies between it and
# HEAD, and the exceptions and the VEX document are read as it has them. In a merge
# request's pipeline it is GitLab's CI_MERGE_REQUEST_DIFF_BASE_SHA, the target branch
# where the merge request left it. A maintainer's batch* branch runs no merge request
# pipeline (the workflow rules of .gitlab-ci.yml): its branch pipeline is the one GitLab
# holds its merge request to, and there the base is where the branch leaves master -
# master fetched by name, as release-chain.py's check_tag fetches it, and the merge base
# of HEAD and it. Anything else cannot be judged, and says so (exit 2, never a pass).

TARGET_BRANCH = "master"
TARGET_REF = "refs/remotes/origin/" + TARGET_BRANCH
FULL_SHA = re.compile(r"^[0-9a-f]{40}(?:[0-9a-f]{24})?$")


def change_base(root, environ=None, run=None):
    """(base commit, how it was found, None), or (None, None, the problem)."""
    environ = os.environ if environ is None else environ
    run = run or (lambda arguments: _git(root, *arguments))

    def said(answer):
        return (answer.stderr or answer.stdout or b"").decode("utf-8", "replace").strip()[:300] or "exit %d" % answer.returncode

    given = (environ.get("CI_MERGE_REQUEST_DIFF_BASE_SHA") or "").strip()
    if given:
        if not FULL_SHA.match(given):
            return None, None, "CI_MERGE_REQUEST_DIFF_BASE_SHA is %r, not a commit" % given
        return given, "the merge request's diff base, CI_MERGE_REQUEST_DIFF_BASE_SHA", None
    if environ.get("CI_PIPELINE_SOURCE") == "merge_request_event" or environ.get("CI_MERGE_REQUEST_IID"):
        # A merge request may target another branch than master: its base is GitLab's
        # to give, never guessed.
        return None, None, "a merge request's pipeline without CI_MERGE_REQUEST_DIFF_BASE_SHA: its base cannot be told"
    shallow = run(["rev-parse", "--is-shallow-repository"])
    if shallow.returncode != 0:
        return None, None, "this is not a git checkout (git rev-parse: %s)" % said(shallow)
    if shallow.stdout.decode("utf-8", "replace").strip() == "true":
        # A shallow boundary can hide where the branch left master.
        return None, None, "this clone is shallow: the job needs the history (GIT_DEPTH 0) to find where it leaves %s" % TARGET_BRANCH
    fetched = run(["fetch", "--no-tags", "origin", "+refs/heads/%s:%s" % (TARGET_BRANCH, TARGET_REF)])
    if fetched.returncode != 0:
        return None, None, "%s could not be fetched from origin: %s; retry the job" % (TARGET_BRANCH, said(fetched))
    tip = run(["rev-parse", "--verify", "--quiet", TARGET_REF + "^{commit}"])
    merged = run(["merge-base", "HEAD", TARGET_REF])
    base = merged.stdout.decode("utf-8", "replace").strip() if merged.returncode == 0 else ""
    if not FULL_SHA.match(base):
        return None, None, "HEAD and %s have no merge base (git merge-base: %s)" % (TARGET_BRANCH, said(merged))
    at = tip.stdout.decode("utf-8", "replace").strip() if tip.returncode == 0 else "?"
    return base, "where HEAD leaves %s: git merge-base HEAD %s, with %s fetched now at %s%s" % (
        TARGET_BRANCH, TARGET_REF, TARGET_BRANCH, at, " (its tip)" if at == base else ""), None


def change_base_failures():
    """The self-test's cases for change_base, against repositories made here: an origin
    with master, a clone on a branch after it, a shallow clone and a merge request's
    environment. Offline: origin is a directory."""
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
        with open(os.path.join(where, name), "w", encoding="utf-8", newline="\n") as f:
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
        commit(clone, "b.txt", "the batch's change\n")
        # master moves on after the branch left it: the base is still where it left.
        moved = commit(origin, "c.txt", "master's later change\n")
        environ = {"CI_PIPELINE_SOURCE": "push", "CI_COMMIT_BRANCH": "batch99"}
        base, how, problem = change_base(clone, environ)
        expect(problem is None and base == forked, "a batch branch's base is where it leaves master, fetched now: %r %r %r"
               % (base, how, problem))
        expect(problem is None and moved in (how or ""), "and the base says which master it read: %r" % how)
        expect(g(clone, "rev-parse", TARGET_REF) == moved, "master is fetched by name into %s" % TARGET_REF)
        given = "a" * 40
        expect(change_base(clone, dict(environ, CI_MERGE_REQUEST_DIFF_BASE_SHA=given))[:1] == (given,),
               "a merge request's pipeline takes CI_MERGE_REQUEST_DIFF_BASE_SHA")
        expect(change_base(clone, {"CI_MERGE_REQUEST_DIFF_BASE_SHA": "HEAD~1"})[2] is not None,
               "a CI_MERGE_REQUEST_DIFF_BASE_SHA that is not a commit is refused")
        expect(change_base(clone, {"CI_PIPELINE_SOURCE": "merge_request_event"})[2] is not None,
               "a merge request's pipeline without its diff base is refused, not judged against master")
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
    except (OSError, subprocess.CalledProcessError) as e:
        failures.append("change_base's repositories could not be made: %s %s" % (e, getattr(e, "stderr", "") or ""))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return failures


def change_gate_failures():
    """The self-test's cases for what a change may not add (change_problems) and for the
    change's own exceptions in whole-tree mode (merge_request_evaluate), against a
    repository made here: offline."""
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    # The markers, in pieces, as SUPPRESSION_MARKERS spells them.
    sem, lint, suppress = "no" + "sem" + "grep", "NO" + "LINT", "cppcheck-" + "suppress"
    for text, count in (("x = 1  # %s\n" % sem, 1), ("x = 1  # %s: python.rule\n" % sem.upper(), 1),
                        ("x = 1  # %s\n" % sem[:5], 1), ("// %s\n// %s\n" % (sem, sem), 2),
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
            excluded = re.search(r'^\s*SAST_EXCLUDED_PATHS:\s*"([^"]*)"', f.read(), re.MULTILINE)
    except OSError:
        excluded = None
    excluded = [p.strip() for p in excluded.group(1).split(",")] if excluded else []
    for path in GENERATED_DOCUMENTS:
        expect(path in excluded, "%s is in semgrep-sast's SAST_EXCLUDED_PATHS (.gitlab/ci/gitlab-security.yml), or its "
                                 "markers would count" % path)
    expect(BASELINES == findings_module.BASELINES, "the baselines this gate holds are the ones the source scans read: %s, %s"
           % (BASELINES, findings_module.BASELINES))

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

    def verdict(base, severity="Medium"):
        """(exit, what it said), the report holding one strcpy at src/a.cpp:2 of severity."""
        if os.path.isdir(artifacts):
            shutil.rmtree(artifacts)
        os.makedirs(artifacts)
        with open(os.path.join(artifacts, GITLAB_SAST_REPORT), "w", encoding="utf-8") as f:
            json.dump({"version": "15.0.4", "scan": {"status": "success", "type": "sast"},
                       "vulnerabilities": [{"id": "random", "name": "strcpy", "severity": severity, "description": "strcpy",
                                            "identifiers": [{"type": "semgrep_id", "name": "c.strcpy", "value": "c.strcpy"}],
                                            "location": {"file": "src/a.cpp", "start_line": 2}}]}, f)
        said = []
        held = findings_module.ROOT
        findings_module.ROOT = repo
        findings_module._lines_cache.clear()
        findings_module._top_level = None
        try:
            return merge_request_main(repo, artifacts, base, None, today, say=said.append), "\n".join(said)
        finally:
            findings_module.ROOT = held
            findings_module._lines_cache.clear()
            findings_module._top_level = None

    def case(what, start, files, expected, needle=None, renames=(), severity="Medium", unsaid=()):
        change(start, files, renames, what)
        code, said = verdict(start, severity)
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
        # The first commit has no parent to reset to: made by hand.
        for relative, content in {"src/a.cpp": source, "src/b.py": python, "src/c.cpp": library,
                                  "build/ci/semgrep-baseline.txt": baseline, EXCEPTIONS: "exceptions: []\n",
                                  ".gitlab/ci/gitlab-security.yml": "semgrep-sast: {}\n",
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
        # Whole-tree mode: the change's own exceptions accept outside the files it touches.
        fp = None
        code, said = verdict(base, "Critical")
        found = re.search(r"fingerprint ([0-9a-f]{20})", said)
        expect(code == 0 and found is None, "a Critical finding in a file the change does not touch is not its: %s" % said)
        change(base, {".gitlab/ci/gitlab-security.yml": "semgrep-sast: {variables: {}}\n"}, message="the scan altered")
        code, said = verdict(base, "Critical")
        found = re.search(r"fingerprint ([0-9a-f]{20})", said)
        expect(code == 1 and found is not None, "a change that alters the scan answers for every finding: %s" % said)
        fp = found.group(1) if found else "0" * 20
        scan = {".gitlab/ci/gitlab-security.yml": "semgrep-sast: {variables: {}}\n"}
        case("the scan altered, with its own exception for a file it does not touch", base, dict(scan, **{EXCEPTIONS: exception(fp)}),
             0, "accepted by the change's own exception until 2027-03-01", severity="Critical")
        case("the scan altered, with its own exception for a file it touches", base,
             dict(scan, **{EXCEPTIONS: exception(fp), "src/a.cpp": source + "// more\n"}), 1,
             "accepted only by this change's own", severity="Critical")
        case("the scan altered, with its own exception lapsed", base, dict(scan, **{EXCEPTIONS: exception(fp, "2026-09-30")}), 1,
             "fingerprint %s" % fp, severity="Critical")
        # One entry that would accept it, and one without its expiry: a file not well
        # formed accepts nothing, the good entry in it included.
        case("the scan altered, with its own exceptions not well formed", base,
             dict(scan, **{EXCEPTIONS: exception(fp) + exception("d" * 20).split("\n", 1)[1].replace("    expires: 2027-03-01\n", "")}),
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
             dict(scan, **{"src/a.cpp": source + "// more\n"}), 0, severity="Critical")
        expect(verdict("0" * 40)[0] == 2, "a base the clone does not have cannot be checked")
    except (OSError, subprocess.CalledProcessError) as e:
        failures.append("the change gate's repository could not be made: %s %s" % (e, getattr(e, "stderr", "") or ""))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return failures


def merge_request_main(root, artifacts, base, exceptions_text=None, today=None, say=print):
    """The change's gate, from a checkout of its head: 0 it may land, 1 it may not, 2 its
    inputs cannot be read. exceptions_text, when given, stands for the base's file."""
    inputs, problem = merge_request_inputs(root, base)
    if problem:
        say("CANNOT CHECK: " + problem)
        return 2
    problems = merge_request_evaluate(root, artifacts, exceptions_text if exceptions_text is not None else inputs["exceptions"],
                                      inputs["changed"], today, say, inputs["own_exceptions"])
    problems += change_problems(inputs, say)
    for p in problems:
        say("HELD: " + p)
    if problems:
        say("This change waits on the %d line(s) HELD above: a finding of severity Critical or High in what it changes "
            "is fixed, or accepted in %s by a change of its own, landed first, with the reason it is not exploitable, a "
            "reviewer and an expiry; a baseline only shrinks, and no change adds a comment by which a scan skips a "
            "finding; a report that is missing or did not finish is a semgrep-sast job to run again (SECURITY.md, "
            "'Vulnerability management policy'; docs/ContinuousIntegration.md, 'Merge request checks')." % (
                len(problems), EXCEPTIONS))
        return 1
    say("No Critical or High finding of semgrep-sast in what this change touches that the base's %s does not accept, "
        "no baseline grown or a reason taken from it, and no comment added by which a scan skips a finding." % EXCEPTIONS)
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
    if args.merge_request is not None:
        base = args.merge_request
        if base == "auto":
            base, how, problem = change_base(ROOT)
            if problem:
                print("CANNOT CHECK: " + problem)
                return 2
            print("The base: %s, %s." % (base, how))
        return merge_request_main(ROOT, os.path.abspath(args.artifacts or ROOT), base, text, today)
    problems = evaluate(ROOT, os.path.abspath(args.artifacts or ROOT), today, text)
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
