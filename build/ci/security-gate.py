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
  python3 build/ci/security-gate.py --self-test
--artifacts is the directory holding the scans' artifacts as the jobs write them
(security/static-analysis/, security/semgrep/, security/dependency-scan/ and
gl-sast-report.json); the checkout's root by default. Exits 0 when the release may go
on, 1 when it may not, 2 on a usage error.
"""

import argparse
import datetime
import importlib.util
import json
import os
import re
import shutil
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


def gitlab_sast_scan(root, artifacts, accepted):
    """semgrep-sast, GitLab's report: every Critical or High finding, fingerprinted as
    security-findings.py fingerprints its own (the rule, the file, the line's text)."""
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
    blocking, taken = [], []
    for f in found:
        if f["severity"] not in BLOCKING_SEVERITIES:
            continue
        if f["fingerprint"] in excepted:
            taken.append((excepted[f["fingerprint"]], f["report"]))
            continue
        blocking.append("semgrep-sast: %s %s at %s:%d [%s] - fingerprint %s" % (
            f["severity"], f["message"][:160], f["path"], f["line"], f["rule"], f["fingerprint"]))
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
    in_force, lapsed = {}, []
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
    finally:
        shutil.rmtree(work, ignore_errors=True)

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--artifacts", default=None, help="where the scans' artifacts are (default: the checkout's root)")
    parser.add_argument("--exceptions", default=None, help="the exceptions file (default: %s)" % EXCEPTIONS)
    parser.add_argument("--today", default=None, help="the date to judge expiry by (YYYY-MM-DD; default: today, UTC)")
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
