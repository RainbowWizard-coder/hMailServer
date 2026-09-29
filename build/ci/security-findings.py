#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The findings of the two source-scanning jobs, held to a baseline.

The static-analysis job (cppcheck and clang-tidy, build/ci/static-analysis.sh) and
the semgrep job (build/ci/semgrep.sh) hand their tools' raw output to this script,
which:

  * reads each tool's own format - cppcheck's XML, clang-tidy's text, semgrep's
    JSON - into one list of findings, each with its tool, rule, file (relative to
    the repository root), line, severity, message and CWE;
  * gives every finding a fingerprint that survives unrelated edits: a hash of the
    tool, the rule, the file and the text of the flagged line with its whitespace
    collapsed - not its line number - and, where one file has the same rule on two
    identical lines, the order of those lines;
  * compares the fingerprints with the job's baseline (build/ci/<job>-baseline.txt),
    the findings that were already there when the baseline was written;
  * writes a GitLab SAST report per tool (gl-sast-<tool>.json, schema 15) and one
    of every tool's findings (gl-sast-report.json, the job's report: GitLab takes
    one file per report type, and refused the upload of static-analysis's two on
    the first pipeline, "only one file can be sent as raw"), a JUnit
    report (junit.xml: a test case per tool that fails when anything is new, and a
    failing test case per new finding), findings.txt (every finding) and
    new-findings.txt (only those not in the baseline);
  * exits 1 when there is a new finding, so that a job without allow_failure fails
    on new findings only; 2 when a tool's output is missing or says it could not
    analyse the source, which is never a pass.

GitLab's free tier stores the SAST reports and offers them for download, and renders
the JUnit report in the pipeline's Tests tab; docs/ContinuousIntegration.md says
what each tier shows.

The baseline is regenerated with --write-baseline, from a job's artifacts (the raw
outputs are in them) or from a run by hand:

  python3 build/ci/security-findings.py --job static-analysis \\
      --cppcheck security/static-analysis/cppcheck.xml \\
      --clang-tidy security/static-analysis/clang-tidy.txt \\
      --out security/static-analysis --write-baseline

  python3 build/ci/security-findings.py --job semgrep \\
      --semgrep security/semgrep/semgrep.json --remap security/semgrep/remap.json \\
      --out security/semgrep --write-baseline

A baseline line ends with why its finding is known rather than fixed, when the
triage that let it in said so: --reasons names a JSON file of {fingerprint: reason}
and {"rule:<rule>": reason} for the findings new to the baseline, and a reason
already in the baseline stays with its finding when the file is written again.

Run it from the root of the checkout the scan read: the flagged lines are read from
the files, and a finding whose file is not there is fingerprinted without its line.
A baseline is for findings nobody has fixed yet, not for findings nobody has read:
docs/ContinuousIntegration.md, "Security scanning", says how one is triaged.
"""

import argparse
import collections
import datetime
import hashlib
import json
import os
import re
import sys
import uuid
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape, quoteattr

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

BASELINES = {
    "static-analysis": os.path.join("build", "ci", "static-analysis-baseline.txt"),
    "semgrep": os.path.join("build", "ci", "semgrep-baseline.txt"),
}

# The GitLab security report schema this writes (gitlab.com's
# security-report-schemas, sast-report-format.json). The reports were validated
# against it on 25 September 2026.
SCHEMA_VERSION = "15.2.5"
# One namespace for the report's vulnerability ids, so that a finding keeps its id
# from one pipeline to the next (the id is made from the fingerprint).
ID_NAMESPACE = uuid.UUID("7d3c1f0e-2b8a-5c4e-9f61-3a0d8e5b2c77")

SEVERITY_ORDER = ["Critical", "High", "Medium", "Low", "Info", "Unknown"]

# What each tool calls "could not analyse this", which fails the job whatever the
# baseline says: a scan that skipped a file has not said the file is clean.
CPPCHECK_ANALYSIS_ERRORS = {
    "syntaxError", "preprocessorErrorDirective", "internalError", "internalAstError",
    "cppcheckError", "unknownMacro", "instantiationError", "premium-internalError",
}
CLANG_TIDY_ANALYSIS_ERRORS = {"clang-diagnostic-error"}
# Semgrep's names for a file its parser could not read at all, which it reports at
# level "warn" (1.178.0: build/ci/semgrep.sh itself, a "Syntax error" at "warn").
SEMGREP_UNPARSED = {"Lexical error", "Syntax error", "Other syntax error", "AST builder error"}
# A partial parse that leaves more lines of a file unread than this is a hole: a
# construct on a line or two the parser does not know is skipped, but one it loses a
# block to is not (1.178.0 skipped lines 42 to 138 of container-image-sign.sh, all of
# its commands, for one-line case statements, and said "warn").
SEMGREP_PARTIAL_LINES = 10


def now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")


_top_level = None


def relative(path):
    """A tool's path as a path from the repository root, with forward slashes.

    The tools write absolute paths in the checkout they ran in, which is not this
    one when a baseline is made from a job's artifacts: so the path is cut to the
    shortest tail that starts with one of this checkout's top-level entries and
    names a file here. A path with no such tail (a system header) stays as it is.
    """
    global _top_level
    if _top_level is None:
        _top_level = set(os.listdir(ROOT))
    path = path.replace("\\", "/")
    # clang-tidy names a header by the path its includer used: .../DKIM/../../X.h
    path = os.path.normpath(path).replace("\\", "/")
    if not path.startswith("/") and not re.match(r"^[A-Za-z]:", path):
        return path[2:] if path.startswith("./") else path
    parts = [p for p in path.split("/") if p]
    for i in range(len(parts)):
        if parts[i] in _top_level:
            tail = "/".join(parts[i:])
            if os.path.exists(os.path.join(ROOT, tail)):
                return tail
    return path


_lines_cache = {}


def source_line(path, line):
    """The text of one line of a file in the checkout, whitespace collapsed; '' when unknown."""
    if path not in _lines_cache:
        try:
            with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as f:
                _lines_cache[path] = f.read().splitlines()
        except OSError:
            _lines_cache[path] = None
    lines = _lines_cache[path]
    if not lines or line < 1 or line > len(lines):
        return ""
    return " ".join(lines[line - 1].split())


def finding(tool, rule, path, line, column, severity, message, cwes=()):
    return {
        "tool": tool,
        "rule": rule,
        "path": relative(path),
        "line": int(line or 0),
        "column": int(column or 0),
        "severity": severity,
        "message": " ".join((message or "").split()),
        "cwe": sorted({str(c) for c in cwes if c}),
    }


# ---------------------------------------------------------------- readers

def read_cppcheck(path):
    """cppcheck --xml (version 2): one finding per <error>, at its first location."""
    findings, errors = [], []
    # cppcheck's own output, written by this job a moment ago; and the standard
    # library's parser resolves no external entity (Python 3.7.1 on) and is not open
    # to entity expansion with the Expat every supported Python carries.
    root = ET.parse(path).getroot()  # nosemgrep
    severity = {"error": "High", "warning": "Medium", "portability": "Low",
                "performance": "Low", "style": "Low", "information": "Info"}
    for e in root.iter("error"):
        rule = e.get("id", "")
        locations = e.findall("location")
        where = locations[0] if locations else None
        f = finding("cppcheck", rule,
                    where.get("file", "") if where is not None else e.get("file0", ""),
                    where.get("line", 0) if where is not None else 0,
                    where.get("column", 0) if where is not None else 0,
                    severity.get(e.get("severity", ""), "Unknown"),
                    e.get("msg", ""),
                    [e.get("cwe")] if e.get("cwe") else [])
        if rule in CPPCHECK_ANALYSIS_ERRORS:
            errors.append(f)
        elif rule in ("missingIncludeSystem", "checkersReport", "missingInclude",
                      "normalCheckLevelMaxBranches", "toomanyconfigs", "unmatchedSuppression"):
            # What cppcheck says about itself, not about the code.
            continue
        else:
            findings.append(f)
    return findings, errors


CLANG_TIDY_LINE = re.compile(
    r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): (?P<kind>warning|error): "
    r"(?P<message>.*?) \[(?P<rule>[A-Za-z0-9_.,+-]+)\]\s*$")

# clang-tidy's checks carry no CWE; these families map onto one where the mapping is
# plain. The rest are named by their check alone.
CLANG_TIDY_CWE = {
    "clang-analyzer-core.NullDereference": "476",
    "clang-analyzer-core.CallAndMessage": "457",
    "clang-analyzer-core.uninitialized": "457",
    "clang-analyzer-core.DivideZero": "369",
    "clang-analyzer-cplusplus.NewDelete": "416",
    "clang-analyzer-unix.Malloc": "401",
    "clang-analyzer-unix.MismatchedDeallocator": "762",
    "clang-analyzer-security.insecureAPI.strcpy": "120",
    "bugprone-use-after-move": "416",
    "bugprone-sizeof-expression": "467",
    "bugprone-integer-division": "682",
    "bugprone-signed-char-misuse": "704",
    "bugprone-suspicious-memset-usage": "665",
    "bugprone-not-null-terminated-result": "170",
    "bugprone-unsafe-functions": "676",
    "cert-err34-c": "20",
    "cert-err33-c": "252",
    "cert-msc50-cpp": "338",
    "cert-msc51-cpp": "337",
}


def clang_tidy_severity(rule, kind):
    if kind == "error":
        return "High"
    if rule.startswith("clang-analyzer-"):
        return "Medium"
    if rule.startswith(("bugprone-", "cert-", "concurrency-")):
        return "Medium"
    return "Low"


OUTSIDE = collections.Counter()

# The server's sources that a build/generate-*.py writes from a document, a page or
# a catalogue: a finding there is fixed in the generator, and the flagged line
# changes whenever a document does (measured: bugprone-suspicious-missing-comma on
# the help pages' text), so it is counted apart, as OUTSIDE is.
GENERATED = re.compile(r"(^|/)Common/Util/(HelpPages|PortalLanguages|PortalOpenPgp|PortalPage|PortalSmime|Profiles)Data\.cpp$")
GENERATED_SKIPPED = collections.Counter()


def read_clang_tidy(path):
    """clang-tidy's text output: 'file:line:col: warning: message [check]' lines."""
    findings, errors = [], []
    seen = set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = CLANG_TIDY_LINE.match(raw.rstrip("\r\n"))
            if not m:
                continue
            if re.match(r"^(/|\.\./|[A-Za-z]:)", relative(m.group("file"))):
                # The static analyser reports a path through Boost or the C++ library
                # where it ends, in that library's header, whatever the header filter
                # says (measured: Boost.Regex's match_flags.hpp, Asio's resolver).
                # Not this project's code, and not this project's to change.
                OUTSIDE[m.group("rule").split(",")[0]] += 1
                continue
            # A header's finding is reported once by every file that includes it.
            key = (relative(m.group("file")), m.group("line"), m.group("col"), m.group("rule"), m.group("message"))
            if key in seen:
                continue
            seen.add(key)
            # One diagnostic, however many names it has: "[bugprone-unused-return-value,
            # cert-err33-c]" is one check and its CERT alias, and is one finding under
            # the first name (clang-tidy sorts them).
            names = m.group("rule").split(",")
            rule = names[0]
            cwe = []
            for name in names:
                for prefix, number in CLANG_TIDY_CWE.items():
                    if name == prefix or name.startswith(prefix + "."):
                        cwe.append(number)
            f_ = finding("clang-tidy", rule, m.group("file"), m.group("line"), m.group("col"),
                         clang_tidy_severity(rule, m.group("kind")), m.group("message"), cwe)
            if len(names) > 1:
                f_["message"] += " (also %s)" % ", ".join(names[1:])
            if rule in CLANG_TIDY_ANALYSIS_ERRORS or (rule.startswith("clang-diagnostic-") and m.group("kind") == "error"):
                errors.append(f_)
            elif rule.startswith("clang-diagnostic-"):
                # A compiler warning: the build's business (linux-build.sh counts them).
                continue
            else:
                findings.append(f_)
    return findings, errors


def read_semgrep(path, remap=None, rule_prefix=""):
    """semgrep --json: results[] and errors[].

    remap maps a file the pages' scripts were extracted into back to the page.
    rule_prefix is the part of a rule id Semgrep made from the directory the rules
    were loaded from (a random temporary one), which is not part of the rule.
    """
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    severity = {"ERROR": "High", "WARNING": "Medium", "INFO": "Low", "INVENTORY": "Info", "EXPERIMENT": "Info"}
    remap = remap or {}

    def place(file):
        return remap.get(file) or remap.get(file.lstrip("/")) or relative(file)

    def rule_of(check_id):
        if rule_prefix and check_id.startswith(rule_prefix):
            return check_id[len(rule_prefix):]
        # semgrep.sh loads the rules from /tmp/hm-semgrep.XXXXXX/rules.
        return re.sub(r"^tmp\.hm-semgrep\.[A-Za-z0-9]+\.rules\.", "", check_id)

    findings, errors = [], []
    for r in data.get("results", []):
        extra = r.get("extra", {})
        meta = extra.get("metadata", {}) or {}
        listed = meta.get("cwe", [])
        cwes = []
        for c in listed if isinstance(listed, list) else [listed]:
            m = re.match(r"CWE-(\d+)", str(c))
            if m:
                cwes.append(m.group(1))
        findings.append(finding("semgrep", rule_of(r.get("check_id", "")), place(r.get("path", "")),
                                r.get("start", {}).get("line", 0), r.get("start", {}).get("col", 0),
                                severity.get(str(extra.get("severity", "")).upper(), "Unknown"),
                                extra.get("message", ""), cwes))
    for e in data.get("errors", []):
        # A hole in the scan, whatever level Semgrep gives it: a file it could not parse
        # at all, a partial parse that left more than SEMGREP_PARTIAL_LINES lines of a
        # file unread, and a rule that gave up on a file (a timeout). Semgrep reports the
        # first two at level "warn", and the job took them for a scan of the whole file.
        # A partial parse of a line or two - a construct the parser does not know,
        # skipped, and the rest of the file scanned - is not a hole.
        kind = e.get("type")
        typed_spans = kind[1] if isinstance(kind, list) and len(kind) > 1 and isinstance(kind[1], list) else []
        kind = kind[0] if isinstance(kind, list) and kind else kind
        level = str(e.get("level", "error")).lower()
        spans = [s for s in (e.get("spans") or typed_spans) if isinstance(s, dict)] or [{}]
        unread = sum(max(0, (s.get("end") or {}).get("line", 0) - (s.get("start") or {}).get("line", 0) + 1)
                     for s in spans if s.get("start") and s.get("end"))
        if level not in ("error", "fatal") and kind not in ("Timeout", "OutOfMemory") and kind not in SEMGREP_UNPARSED \
                and not (kind == "PartialParsing" and unread > SEMGREP_PARTIAL_LINES):
            continue
        file = e.get("path") or spans[0].get("file") or spans[0].get("path", "")
        said = str(e.get("message", "")).split("\n")[0][:300]
        if kind == "PartialParsing":
            said = "%d lines left unread (%s): %s" % (unread, ", ".join(
                "%s-%s" % ((s.get("start") or {}).get("line"), (s.get("end") or {}).get("line")) for s in spans), said)
        errors.append(finding("semgrep", rule_of(str(e.get("rule_id") or kind or "semgrep-error")), place(file),
                              (spans[0].get("start") or {}).get("line", 0), 0, "Unknown", "%s: %s" % (kind, said)))
    return findings, errors


# ---------------------------------------------------------------- fingerprints and the baseline

def fingerprint_all(findings):
    groups = collections.defaultdict(list)
    for f in findings:
        f["text"] = source_line(f["path"], f["line"])
        groups[(f["tool"], f["rule"], f["path"], f["text"])].append(f)
    for key, members in groups.items():
        members.sort(key=lambda f: (f["line"], f["column"], f["message"]))
        for index, f in enumerate(members):
            material = "|".join(key) + "|%d" % index
            if not f["text"]:
                # No line to read: the message has to stand in for it.
                material += "|" + f["message"]
            f["fingerprint"] = hashlib.sha256(material.encode("utf-8")).hexdigest()[:20]


def read_baseline(path):
    """{fingerprint: [tool, rule, file:line, message, reason if there is one]}."""
    known = {}
    if not os.path.exists(path):
        return known
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            known[fields[0]] = fields[1:]
    return known


def write_baseline(path, job, findings, reasons=None):
    """The baseline, every finding of this run in it. A reason the old file gave a
    finding stays with it; a finding new to it takes its reason from reasons, by
    fingerprint, else by "rule:<rule>"."""
    previous = read_baseline(path)
    reasons = reasons or {}
    body = []
    with_reason = 0
    fresh, fresh_without = 0, 0
    for f in sorted(findings, key=lambda f: (f["path"], f["line"], f["tool"], f["rule"])):
        fields = [f["fingerprint"], f["tool"], f["rule"], "%s:%d" % (f["path"], f["line"]),
                  f["message"][:200].rstrip().replace("\t", " ")]
        old = previous.get(f["fingerprint"])
        if old is not None:
            reason = old[4] if len(old) > 4 else ""
        else:
            reason = reasons.get(f["fingerprint"]) or reasons.get("rule:" + f["rule"]) or ""
            fresh += 1
            fresh_without += 0 if reason.strip() else 1
        reason = " ".join(reason.split())
        if reason:
            fields.append(reason)
            with_reason += 1
        body.append("\t".join(fields))
    lines = [
        "# The findings of the %s job that were already there when this file was written, one per" % job,
        "# line: fingerprint, tool, rule, file:line (where it was then; the fingerprint does not",
        "# use the number), message, and - where its triage said - why it is known rather than",
        "# fixed. The job fails only on a finding whose fingerprint is not here.",
        "# Written by build/ci/security-findings.py --write-baseline; do not edit by hand, except to",
        "# delete the line of a finding that has been fixed. docs/ContinuousIntegration.md,",
        "# \"Security scanning\", says when and how to regenerate it.",
        "#",
        "# %d findings, %d of them with a reason." % (len(findings), with_reason),
    ] + body
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(lines) + "\n")
    return fresh, fresh_without


# ---------------------------------------------------------------- the reports

def sast_report(tool, tool_name, tool_version, findings, started, ended, status, names=None):
    vulnerabilities = []
    for f in findings:
        # A finding's own tool, which is the report's except in the report of them all.
        identifiers = [{
            "type": "%s_rule" % f["tool"].replace("-", "_"),
            "name": "%s %s" % ((names or {}).get(f["tool"], tool_name), f["rule"]),
            "value": f["rule"],
        }]
        for c in f["cwe"]:
            identifiers.append({"type": "cwe", "name": "CWE-%s" % c, "value": c,
                                "url": "https://cwe.mitre.org/data/definitions/%s.html" % c})
        # The fingerprint the baseline and .gitlab/security-exceptions.yml name the
        # finding by, which build/ci/security-gate.py prints for a finding that holds
        # a release.
        identifiers.append({"type": "hmailserver_fingerprint", "name": "fingerprint %s" % f["fingerprint"],
                            "value": f["fingerprint"]})
        vulnerabilities.append({
            "id": str(uuid.uuid5(ID_NAMESPACE, f["fingerprint"])),
            "name": f["rule"],
            "description": f["message"] or f["rule"],
            "severity": f["severity"],
            "identifiers": identifiers,
            "location": {"file": f["path"], "start_line": max(f["line"], 1), "end_line": max(f["line"], 1)},
        })
    return {
        "version": SCHEMA_VERSION,
        "scan": {
            "analyzer": {"id": "hmailserver-%s" % tool, "name": "hMailServer %s job" % tool_name,
                         "version": "1", "vendor": {"name": "hMailServer"}},
            "scanner": {"id": tool, "name": tool_name, "version": tool_version or "unknown",
                        "vendor": {"name": tool_name}},
            "type": "sast",
            "start_time": started,
            "end_time": ended,
            "status": status,
        },
        "vulnerabilities": vulnerabilities,
    }


def junit(job, per_tool, new, errors):
    suites = []
    for tool, findings in per_tool.items():
        tool_new = [f for f in new if f["tool"] == tool]
        tool_errors = [f for f in errors if f["tool"] == tool]
        cases = []
        known = len(findings) - len(tool_new)
        name = "nothing new beyond the baseline (%d known finding(s))" % known
        if tool_new:
            body = "\n".join(describe(f) for f in tool_new[:200])
            cases.append('<testcase classname=%s name=%s><failure message=%s>%s</failure></testcase>' % (
                quoteattr(job + "." + tool), quoteattr(name),
                quoteattr("%d new finding(s)" % len(tool_new)), escape(body)))
        else:
            cases.append('<testcase classname=%s name=%s/>' % (quoteattr(job + "." + tool), quoteattr(name)))
        name = "analysed every file it was given"
        if tool_errors:
            body = "\n".join(describe(f) for f in tool_errors[:200])
            cases.append('<testcase classname=%s name=%s><failure message=%s>%s</failure></testcase>' % (
                quoteattr(job + "." + tool), quoteattr(name),
                quoteattr("%d file(s) or rule(s) not analysed" % len(tool_errors)), escape(body)))
        else:
            cases.append('<testcase classname=%s name=%s/>' % (quoteattr(job + "." + tool), quoteattr(name)))
        for f in tool_new[:500]:
            cases.append('<testcase classname=%s name=%s><failure message=%s>%s</failure></testcase>' % (
                quoteattr(job + "." + tool + ".new"),
                quoteattr("%s at %s:%d" % (f["rule"], f["path"], f["line"])),
                quoteattr(f["severity"] + ": " + f["message"][:300]), escape(describe(f))))
        failures = sum(1 for c in cases if "<failure" in c)
        suites.append('<testsuite name=%s tests="%d" failures="%d">\n%s\n</testsuite>' % (
            quoteattr("%s: %s" % (job, tool)), len(cases), failures, "\n".join(cases)))
    return '<?xml version="1.0" encoding="UTF-8"?>\n<testsuites>\n%s\n</testsuites>\n' % "\n".join(suites)


def describe(f):
    cwe = (" CWE-" + ",".join(f["cwe"])) if f["cwe"] else ""
    return "%s:%d  %s  %s [%s]%s" % (f["path"], f["line"], f["severity"], f["message"][:240], f["rule"], cwe)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--job", required=True, choices=sorted(BASELINES))
    parser.add_argument("--cppcheck", help="cppcheck --xml output")
    parser.add_argument("--clang-tidy", dest="clang_tidy", help="clang-tidy's output, every file's")
    parser.add_argument("--semgrep", help="semgrep --json output")
    parser.add_argument("--remap", help="JSON {extracted file: page it came from}, for semgrep")
    parser.add_argument("--rule-prefix", default="", help="the prefix Semgrep gave the rule ids, to take off")
    parser.add_argument("--versions", help="JSON {tool: version}, for the reports")
    parser.add_argument("--started", help="when the scan started (UTC, YYYY-MM-DDTHH:MM:SS)")
    parser.add_argument("--out", required=True, help="the directory the reports go to")
    parser.add_argument("--baseline", help="the baseline file (default: the job's, under build/ci)")
    parser.add_argument("--reasons", help="JSON {fingerprint or \"rule:<rule>\": reason}, for --write-baseline")
    parser.add_argument("--write-baseline", action="store_true",
                        help="rewrite the baseline with every finding of this run, then report against it")
    args = parser.parse_args()

    os.chdir(ROOT)
    started = args.started or now()
    versions = {}
    if args.versions and os.path.exists(args.versions):
        with open(args.versions, encoding="utf-8") as f:
            versions = json.load(f)

    inputs = []
    if args.job == "static-analysis":
        inputs = [("cppcheck", "Cppcheck", args.cppcheck, read_cppcheck),
                  ("clang-tidy", "clang-tidy", args.clang_tidy, read_clang_tidy)]
    else:
        remap = {}
        if args.remap and os.path.exists(args.remap):
            with open(args.remap, encoding="utf-8") as f:
                remap = json.load(f)
        inputs = [("semgrep", "Semgrep", args.semgrep, lambda p: read_semgrep(p, remap, args.rule_prefix))]

    missing = [tool for tool, _, path, _ in inputs if not path or not os.path.exists(path)]
    if missing:
        print("No output from %s: the scan did not run to its end, and a scan that did not run found nothing"
              " only because it looked at nothing." % ", ".join(missing))
        return 2

    per_tool, errors, all_findings = collections.OrderedDict(), [], []
    for tool, _, path, reader in inputs:
        found, errs = reader(path)
        for f in found:
            if GENERATED.search(f["path"]):
                GENERATED_SKIPPED[f["path"].rsplit("/", 1)[-1]] += 1
        found = [f for f in found if not GENERATED.search(f["path"])]
        per_tool[tool] = found
        errors.extend(errs)
        all_findings.extend(found)
    fingerprint_all(all_findings)
    for e in errors:
        e.setdefault("fingerprint", "")

    baseline_path = args.baseline or os.path.join(ROOT, BASELINES[args.job])
    if args.write_baseline:
        # Then on as usual, against the file just written: the reports are made, and
        # nothing is new.
        reasons = {}
        if args.reasons:
            with open(args.reasons, encoding="utf-8") as f:
                reasons = json.load(f)
        fresh, fresh_without = write_baseline(baseline_path, args.job, all_findings, reasons)
        print("Wrote %d finding(s) to %s; %d new to it, %d of those without a reason." % (
            len(all_findings), os.path.relpath(baseline_path, ROOT), fresh, fresh_without))

    known = read_baseline(baseline_path)
    new = [f for f in all_findings if f["fingerprint"] not in known]
    gone = len(set(known) - {f["fingerprint"] for f in all_findings})

    os.makedirs(args.out, exist_ok=True)
    ended = now()
    for tool, name, _, _ in inputs:
        tool_errors = [e for e in errors if e["tool"] == tool]
        report = sast_report(tool, name, versions.get(tool), per_tool[tool], started, ended,
                             "failure" if tool_errors else "success")
        with open(os.path.join(args.out, "gl-sast-%s.json" % tool), "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
    names = {tool: name for tool, name, _, _ in inputs}
    report = sast_report(args.job, " and ".join(names.values()),
                         "; ".join("%s %s" % (names[t], versions.get(t) or "unknown") for t in names),
                         all_findings, started, ended, "failure" if errors else "success", names)
    with open(os.path.join(args.out, "gl-sast-report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    with open(os.path.join(args.out, "junit.xml"), "w", encoding="utf-8") as f:
        f.write(junit(args.job, per_tool, new, errors))
    order = {s: i for i, s in enumerate(SEVERITY_ORDER)}
    ranked = sorted(all_findings, key=lambda f: (order.get(f["severity"], 9), f["path"], f["line"]))
    with open(os.path.join(args.out, "findings.txt"), "w", encoding="utf-8") as f:
        f.write("".join(describe(x) + "\n" for x in ranked))
    with open(os.path.join(args.out, "new-findings.txt"), "w", encoding="utf-8") as f:
        f.write("".join(describe(x) + "\n" for x in sorted(new, key=lambda f: (order.get(f["severity"], 9), f["path"], f["line"]))))

    print("")
    print("== %s: findings by tool and rule" % args.job)
    for tool, found in per_tool.items():
        counts = collections.Counter(f["rule"] for f in found)
        by_severity = collections.Counter(f["severity"] for f in found)
        print("%s: %d finding(s) (%s)" % (tool, len(found), ", ".join(
            "%s %d" % (s, by_severity[s]) for s in SEVERITY_ORDER if by_severity[s])))
        for rule, n in counts.most_common(25):
            print("  %6d  %s" % (n, rule))
    if OUTSIDE:
        print("Not counted: %d finding(s) in headers outside the repository (%s)." % (
            sum(OUTSIDE.values()), ", ".join("%s %d" % kv for kv in OUTSIDE.most_common(5))))
    if GENERATED_SKIPPED:
        print("Not counted: %d finding(s) in generated sources, whose generators are the place to fix them (%s)." % (
            sum(GENERATED_SKIPPED.values()), ", ".join("%s %d" % kv for kv in GENERATED_SKIPPED.most_common())))
    print("")
    print("Baseline %s: %d known; %d new; %d in the baseline no longer found (fixed, or moved"
          " past recognition - delete their lines, or regenerate)." % (
              os.path.relpath(baseline_path, ROOT), len(known), len(new), gone))
    if new:
        print("")
        print("NEW findings (%d):" % len(new))
        for f in sorted(new, key=lambda f: (order.get(f["severity"], 9), f["path"], f["line"]))[:100]:
            print("  " + describe(f))
        if len(new) > 100:
            print("  ... and %d more in new-findings.txt" % (len(new) - 100))
    if errors:
        print("")
        print("NOT ANALYSED (%d) - the scan has holes, which no baseline excuses:" % len(errors))
        for e in errors[:60]:
            print("  " + describe(e))
        return 2
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
