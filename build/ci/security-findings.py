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
  * with --exceptions, reads master's .gitlab/security-exceptions.yml (the job's
    script fetches it; never the commit's own copy, so that a change cannot accept
    its own finding) with build/ci/security-gate.py's own reader, and takes a finding
    new to the baseline that an exception in force accepts - one naming this job as
    its scan and the finding's fingerprint, reviewed, and not past its expiry - as
    accepted rather than new;
  * writes a GitLab SAST report per tool (gl-sast-<tool>.json, schema 15) and one
    of every tool's findings (gl-sast-report.json, the job's report: GitLab takes
    one file per report type, and refused the upload of static-analysis's two on
    the first pipeline, "only one file can be sent as raw"), a JUnit
    report (junit.xml: a test case per tool that fails when anything is new, and a
    failing test case per new finding), findings.txt (every finding),
    new-findings.txt (those neither in the baseline nor accepted) and
    accepted-findings.txt (those not in the baseline that an exception accepts);
  * exits 1 when there is a new finding no exception accepts, so that a job without
    allow_failure fails on those only; 2 when a tool's output is missing or says it
    could not analyse the source, or the exceptions file given cannot be read,
    which is never a pass.

GitLab's free tier stores the SAST reports and offers them for download, and renders
the JUnit report in the pipeline's Tests tab; docs/ContinuousIntegration.md says
what each tier shows.

A baseline only shrinks. It is rewritten with --write-baseline, from a job's
artifacts (the raw outputs are in them) or from a run by hand:

  python3 build/ci/security-findings.py --job static-analysis \\
      --cppcheck security/static-analysis/cppcheck.xml \\
      --clang-tidy security/static-analysis/clang-tidy.txt \\
      --out security/static-analysis --write-baseline

  python3 build/ci/security-findings.py --job semgrep \\
      --semgrep security/semgrep/semgrep.json --remap security/semgrep/remap.json \\
      --out security/semgrep --write-baseline

which keeps every entry this run still finds, at its line now, drops the ones it no
longer finds, and adds none: a finding new to the baseline is fixed, or accepted in
master's .gitlab/security-exceptions.yml by a change of its own landed first
(SECURITY.md, "Vulnerability management policy"), and the rewrite lists each one it
left out. A baseline line ends with why its finding is known rather than fixed, when
its triage said so: a reason already in the baseline stays with its finding, and
--reasons names a JSON file of {fingerprint: reason} and {"rule:<rule>": reason}
that gives one to a kept entry that has none.

Run it from the root of the checkout the scan read: the flagged lines are read from
the files, and a finding whose file is not there is fingerprinted without its line.
A baseline is for findings nobody has fixed yet, not for findings nobody has read:
docs/ContinuousIntegration.md, "Security scanning", says how one is triaged.

  --exceptions FILE   the exceptions file to judge new findings by (the job scripts
                      give master's copy, which they fetch before the scan)
  --today YYYY-MM-DD  the day an exception's expiry is judged by (default: today, UTC)
  --self-test         the baseline's rewrite and the exceptions, against a tree, a
                      tool's output and exceptions files it makes for itself; no network
"""

import argparse
import collections
import contextlib
import datetime
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import sys
import tempfile
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
# its commands, for one-line case statements, and said "warn"). The lines are counted
# once each: Semgrep names a line as often as it failed on it (1.178.0: line 276 of
# build/linux-tu-census.sh three times), and a line named three times is one line.
SEMGREP_PARTIAL_LINES = 10
# Semgrep's names for a rule that did not finish with a file, which it reports at level
# "warn": its own list of the errors that leave a file not fully scanned (semgrep/error.py,
# is_scan_failure: a timeout, memory, the stack, a taint fixpoint that timed out), a rule
# that failed while matching, and one that stopped at too many matches. 1.178.0 writes
# "Out of memory" (measured, with --max-memory 20), not "OutOfMemory", which this read
# before and which matched nothing.
SEMGREP_GAVE_UP = {"Timeout", "Out of memory", "Stack overflow", "FixpointTimeout",
                   "Internal matching error", "Too many matches"}


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
        # file unread, and a rule that did not finish with a file (SEMGREP_GAVE_UP).
        # Semgrep reports all three at level "warn", and the job took them for a scan of
        # the whole file. A partial parse of a line or two - a construct the parser does
        # not know, skipped, and the rest of the file scanned - is not a hole.
        kind = e.get("type")
        typed_spans = kind[1] if isinstance(kind, list) and len(kind) > 1 and isinstance(kind[1], list) else []
        kind = kind[0] if isinstance(kind, list) and kind else kind
        level = str(e.get("level", "error")).lower()
        spans = [s for s in (e.get("spans") or typed_spans) if isinstance(s, dict)] or [{}]
        # The lines the spans cover, each counted once: a span's end line is its last
        # (1.178.0: lines 42 to 138 of container-image-sign.sh, "end" 138, column 9),
        # and spans repeat and overlap.
        ranges = sorted((s["start"]["line"], s["end"]["line"]) for s in spans
                        if isinstance(s.get("start"), dict) and isinstance(s.get("end"), dict)
                        and isinstance(s["start"].get("line"), int) and isinstance(s["end"].get("line"), int)
                        and s["end"]["line"] >= s["start"]["line"])
        unread, reach, merged = 0, 0, []
        for first, last in ranges:
            if last <= reach:
                continue
            first = max(first, reach + 1)
            if merged and first == merged[-1][1] + 1:
                merged[-1][1] = last
            else:
                merged.append([first, last])
            unread += last - first + 1
            reach = last
        if level not in ("error", "fatal") and kind not in SEMGREP_GAVE_UP and kind not in SEMGREP_UNPARSED \
                and not (kind == "PartialParsing" and unread > SEMGREP_PARTIAL_LINES):
            continue
        file = e.get("path") or spans[0].get("file") or spans[0].get("path", "")
        said = str(e.get("message", "")).split("\n")[0][:300]
        if kind == "PartialParsing":
            said = "%d lines left unread (%s): %s" % (unread, ", ".join("%d-%d" % (a, b) for a, b in merged), said)
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


def parse_baseline(text):
    """{fingerprint: [tool, rule, file:line, message, reason if there is one]}, from a
    baseline's text: what the job reads, and what build/ci/security-gate.py compares
    between a change's base and its head."""
    known = {}
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        known[fields[0]] = fields[1:]
    return known


def read_baseline(path):
    """parse_baseline of the file at path; {} when there is none."""
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as f:
        return parse_baseline(f.read())


BASELINE_HEADER = [
    "# The findings of the %s job that were already there when this file was written, one per",
    "# line: fingerprint, tool, rule, file:line (where it was then; the fingerprint does not",
    "# use the number), message, and - where its triage said - why it is known rather than",
    "# fixed. The job fails on a finding whose fingerprint is not here, unless master's",
    "# .gitlab/security-exceptions.yml accepts it.",
    "# The list only shrinks: build/ci/security-findings.py --write-baseline keeps the findings",
    "# still found, drops the ones gone and adds none. By hand, delete the line of a finding",
    "# that has been fixed, or end a line that has no reason with a tab and the reason its",
    "# triage gave; nothing else. docs/ContinuousIntegration.md, \"Security scanning\", says how.",
    "#",
]


def write_baseline(path, job, findings, reasons=None):
    """The baseline again, as this run finds it, and never longer than it was: every entry
    of the old file that this run still finds, at its line now, with the reason the old
    file gave it or, when it gave none, the one reasons gives it (by fingerprint, else by
    "rule:<rule>"); no entry this run no longer finds; and no finding the old file did not
    hold. {"kept": the findings kept, "dropped": the old entries left out as [fingerprint,
    tool, rule, file:line, ...], "left_out": the findings new to the baseline, which it does
    not take, "reasons_added": how many kept entries took a reason from reasons}."""
    previous = read_baseline(path)
    reasons = reasons or {}
    body, kept, left_out = [], [], []
    with_reason, added = 0, 0
    for f in sorted(findings, key=lambda f: (f["path"], f["line"], f["tool"], f["rule"])):
        old = previous.get(f["fingerprint"])
        if old is None:
            left_out.append(f)
            continue
        fields = [f["fingerprint"], f["tool"], f["rule"], "%s:%d" % (f["path"], f["line"]),
                  f["message"][:200].rstrip().replace("\t", " ")]
        reason = old[4] if len(old) > 4 else ""
        if not reason.strip():
            reason = reasons.get(f["fingerprint"]) or reasons.get("rule:" + f["rule"]) or ""
            added += 1 if reason.strip() else 0
        reason = " ".join(reason.split())
        if reason:
            fields.append(reason)
            with_reason += 1
        body.append("\t".join(fields))
        kept.append(f)
    found = {f["fingerprint"] for f in findings}
    dropped = [[fingerprint] + fields for fingerprint, fields in previous.items() if fingerprint not in found]
    lines = [BASELINE_HEADER[0] % job] + BASELINE_HEADER[1:] + [
        "# %d findings, %d of them with a reason." % (len(kept), with_reason),
    ] + body
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(lines) + "\n")
    return {"kept": kept, "dropped": dropped, "left_out": left_out, "reasons_added": added}


# ---------------------------------------------------------------- the exceptions

# An exception this close to its expiry is named in the job's log, which still passes:
# once it lapses the finding fails the job again, on every branch.
EXPIRY_WARNING_DAYS = 30


_gate = None


def _load_gate():
    """build/ci/security-gate.py as a module, for its reader of the exceptions file: one
    reader, so that what the release's gate accepts and what this job accepts cannot
    differ. It is loaded when an exceptions file is given, not before, because it loads
    this file in turn."""
    global _gate
    if _gate is None:
        spec = importlib.util.spec_from_file_location(
            "security_gate", os.path.join(os.path.dirname(os.path.abspath(__file__)), "security-gate.py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _gate = module
    return _gate


def read_exceptions(path, job, today, say=print):
    """({fingerprint: exception}, problem): the exceptions in force today that name this job
    as their scan, read from path with security-gate.py's check_exceptions and _in_force,
    or ({}, what is wrong) when the file cannot be read or is not well formed - which
    accepts nothing, and fails the job (exit 2) rather than letting a finding through or
    holding it for a reason the log does not give."""
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except (OSError, UnicodeDecodeError) as e:
        return {}, "the exceptions file %s cannot be read: %s" % (path, e)
    gate = _load_gate()
    entries, problems = gate.check_exceptions(text)
    if problems:
        return {}, "the exceptions file %s is not well formed, so nothing in it is accepted: %s" % (
            path, "; ".join(problems))
    in_force, problems = gate._in_force(entries, today, say)
    if problems:
        return {}, "the exceptions file %s has an entry that cannot be in force, so nothing in it is accepted: %s" % (
            path, "; ".join(problems))
    accepted = {}
    for entry in in_force.get(job, []):
        accepted[entry["finding"]] = entry
        # check_exceptions has held the date to YYYY-MM-DD.
        if (datetime.date.fromisoformat(entry["expires"]) - today).days <= EXPIRY_WARNING_DAYS:
            say("WARNING: the exception for %s %s (%s) expires on %s; after that its finding fails this job again "
                "unless it is fixed or the exception is reviewed again." % (job, entry["finding"], entry["what"],
                                                                           entry["expires"]))
    return accepted, None


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


def junit(job, per_tool, new, errors, accepted=(), unjudged=None):
    """unjudged: why the exceptions file could not be read, which fails every tool's
    first test case, since nothing new could be judged."""
    suites = []
    for tool, findings in per_tool.items():
        tool_new = [f for f in new if f["tool"] == tool]
        tool_accepted = [f for f in accepted if f["tool"] == tool]
        tool_errors = [f for f in errors if f["tool"] == tool]
        cases = []
        known = len(findings) - len(tool_new) - len(tool_accepted)
        name = "nothing new beyond the baseline (%d known finding(s), %d accepted by an exception)" % (
            known, len(tool_accepted))
        if unjudged:
            cases.append('<testcase classname=%s name=%s><failure message=%s>%s</failure></testcase>' % (
                quoteattr(job + "." + tool), quoteattr("the exceptions file was read"),
                quoteattr("the exceptions file cannot be read"), escape(unjudged)))
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


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv == ["--self-test"]:
        return self_test()
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
    parser.add_argument("--reasons", help="JSON {fingerprint or \"rule:<rule>\": reason}, for --write-baseline: the "
                                          "reason of a kept entry that has none")
    parser.add_argument("--write-baseline", action="store_true",
                        help="rewrite the baseline - the entries this run still finds, none it no longer finds and none "
                             "new to it - then report against it")
    parser.add_argument("--exceptions", help="the exceptions file a finding new to the baseline may be accepted by "
                                             "(the job scripts give master's .gitlab/security-exceptions.yml)")
    parser.add_argument("--today", help="the day an exception's expiry is judged by (YYYY-MM-DD; default: today, UTC)")
    args = parser.parse_args(argv)

    today = datetime.datetime.now(datetime.timezone.utc).date()
    if args.today:
        try:
            today = datetime.date.fromisoformat(args.today)
        except ValueError:
            print("--today is not a date (YYYY-MM-DD): %s" % args.today)
            return 2

    os.chdir(ROOT)
    started = args.started or now()
    versions = {}
    if args.versions and os.path.exists(args.versions):
        with open(args.versions, encoding="utf-8") as f:
            versions = json.load(f)

    # Read before the findings, so that a file which cannot be read is said at the top of
    # the log; the reports are still written, and the job fails at the end (exit 2).
    accepted_by, unjudged = {}, None
    if args.exceptions:
        accepted_by, unjudged = read_exceptions(args.exceptions, args.job, today)
        if not unjudged:
            print("Exceptions: %s, read as security-gate.py reads it; %d in force today (%s) for the %s job." % (
                args.exceptions, len(accepted_by), today.isoformat(), args.job))

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

    order = {s: i for i, s in enumerate(SEVERITY_ORDER)}

    def ranked(findings):
        return sorted(findings, key=lambda f: (order.get(f["severity"], 9), f["path"], f["line"]))

    baseline_path = args.baseline or os.path.join(ROOT, BASELINES[args.job])
    if args.write_baseline:
        # Then on as usual, against the file just written: the reports are made, and a
        # finding it left out is new, or accepted, as it was before the rewrite.
        reasons = {}
        if args.reasons:
            with open(args.reasons, encoding="utf-8") as f:
                reasons = json.load(f)
        rewritten = write_baseline(baseline_path, args.job, all_findings, reasons)
        print("Wrote %d finding(s) to %s, %d of them given their reason by --reasons. Dropped %d no longer found; "
              "left out %d new to it, which a baseline never takes: each is fixed, or accepted in master's "
              ".gitlab/security-exceptions.yml by a change of its own landed first." % (
                  len(rewritten["kept"]), os.path.relpath(baseline_path, ROOT), rewritten["reasons_added"],
                  len(rewritten["dropped"]), len(rewritten["left_out"])))
        for entry in rewritten["dropped"]:
            print("  dropped   %s" % "  ".join(entry[:4]))
        for f in ranked(rewritten["left_out"]):
            print("  left out  %s - fingerprint %s" % (describe(f), f["fingerprint"]))

    known = read_baseline(baseline_path)
    beyond = [f for f in all_findings if f["fingerprint"] not in known]
    accepted = [f for f in beyond if f["fingerprint"] in accepted_by]
    new = [f for f in beyond if f["fingerprint"] not in accepted_by]
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
        f.write(junit(args.job, per_tool, new, errors, accepted, unjudged))
    with open(os.path.join(args.out, "findings.txt"), "w", encoding="utf-8") as f:
        f.write("".join(describe(x) + "\n" for x in ranked(all_findings)))
    with open(os.path.join(args.out, "new-findings.txt"), "w", encoding="utf-8") as f:
        f.write("".join("%s - fingerprint %s\n" % (describe(x), x["fingerprint"]) for x in ranked(new)))
    with open(os.path.join(args.out, "accepted-findings.txt"), "w", encoding="utf-8") as f:
        f.write("".join("%s - fingerprint %s, accepted until %s (%s): %s\n" % (
            describe(x), x["fingerprint"], accepted_by[x["fingerprint"]]["expires"],
            accepted_by[x["fingerprint"]]["reviewed-by"], accepted_by[x["fingerprint"]]["what"]) for x in ranked(accepted)))

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
    print("Baseline %s: %d known; %d beyond it, %d of those accepted by an exception and %d new; %d in the baseline "
          "no longer found (fixed, or moved past recognition - delete their lines, or regenerate)." % (
              os.path.relpath(baseline_path, ROOT), len(known), len(beyond), len(accepted), len(new), gone))
    if accepted:
        print("")
        print("ACCEPTED by an exception in %s (%d) - not in the baseline, and declared not exploitable:" % (
            args.exceptions, len(accepted)))
        for f in ranked(accepted)[:100]:
            entry = accepted_by[f["fingerprint"]]
            print("  %s - fingerprint %s, accepted until %s (%s): %s" % (
                describe(f), f["fingerprint"], entry["expires"], entry["reviewed-by"], entry["what"]))
        if len(accepted) > 100:
            print("  ... and %d more in accepted-findings.txt" % (len(accepted) - 100))
    if new:
        print("")
        print("NEW findings (%d) - each is fixed, or accepted in master's .gitlab/security-exceptions.yml by a change "
              "of its own landed first (SECURITY.md, \"Vulnerability management policy\"):" % len(new))
        for f in ranked(new)[:100]:
            print("  %s - fingerprint %s" % (describe(f), f["fingerprint"]))
        if len(new) > 100:
            print("  ... and %d more in new-findings.txt" % (len(new) - 100))
    status = 0
    if errors:
        print("")
        print("NOT ANALYSED (%d) - the scan has holes, which no baseline excuses:" % len(errors))
        for e in errors[:60]:
            print("  " + describe(e))
        status = 2
    if unjudged:
        print("")
        print("NOT JUDGED: %s. Nothing beyond the baseline could be accepted, so the job cannot pass; the file the "
              "job reads is master's, fetched by the job's script." % unjudged)
        status = 2
    if status:
        return status
    return 1 if new else 0


# ---------------------------------------------------------------- the self-test

def self_test():
    """The rewrite of a baseline and the exceptions, against a tree, Semgrep output and
    exceptions files made here. No network, and nothing of the checkout is written."""
    global ROOT, _top_level
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    held_root, held_cwd = ROOT, os.getcwd()
    work = tempfile.mkdtemp(prefix="security-findings-")
    try:
        tree = os.path.join(work, "tree")
        os.makedirs(os.path.join(tree, "src"))
        with open(os.path.join(tree, "src", "a.py"), "w", encoding="utf-8", newline="\n") as f:
            f.write("import os\nos.system(command)\neval(text)\nexec(code)\npickle.loads(blob)\nsubprocess.call(argv)\n")
        ROOT = tree
        _top_level = None
        _lines_cache.clear()

        def fingerprint(line):
            f = finding("semgrep", "rule-%d" % line, "src/a.py", line, 1, "High", "m%d" % line)
            fingerprint_all([f])
            return f["fingerprint"]

        known2, known6, new3, new4, gone = fingerprint(2), fingerprint(6), fingerprint(3), fingerprint(4), "f" * 20
        baseline = os.path.join(work, "baseline.txt")
        old_reason = "Triaged 1 October 2026: the command is the script's own constant."

        def lay_baseline():
            with open(baseline, "w", encoding="utf-8", newline="\n") as f:
                f.write("# a baseline\n#\n# 3 findings, 1 of them with a reason.\n")
                f.write("\t".join([known2, "semgrep", "rule-2", "src/a.py:9", "m2", old_reason]) + "\n")
                f.write("\t".join([gone, "semgrep", "rule-5", "src/a.py:5", "m5"]) + "\n")
                f.write("\t".join([known6, "semgrep", "rule-6", "src/a.py:6", "m6"]) + "\n")

        def exceptions(*entries, **kw):
            path = os.path.join(work, kw.get("name", "exceptions.yml"))
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("exceptions:\n" + "".join(
                    "  - scan: %s\n    finding: %s\n    what: 'eval of a fixed string'\n"
                    "    reason: 'Read in the code: the text is a literal of the script and never comes from a peer.'\n"
                    "    reviewed-by: Progressiverobot\n    reviewed: %s\n    expires: %s\n" % e for e in entries))
            return path

        def run(found, *extra, **kw):
            semgrep_json = os.path.join(work, "semgrep.json")
            with open(semgrep_json, "w", encoding="utf-8") as f:
                json.dump({"results": [{"check_id": "rule-%d" % n, "path": "src/a.py", "start": {"line": n, "col": 1},
                                        "extra": {"severity": "ERROR", "message": "m%d" % n, "metadata": {}}}
                                       for n in found], "errors": kw.get("holes", [])}, f)
            out = os.path.join(work, "out")
            shutil.rmtree(out, ignore_errors=True)
            said = io.StringIO()
            with contextlib.redirect_stdout(said):
                code = main(["--job", "semgrep", "--semgrep", semgrep_json, "--out", out, "--baseline", baseline,
                             "--today", "2026-10-01"] + list(extra))
            files = {}
            for name in ("new-findings.txt", "accepted-findings.txt", "junit.xml"):
                try:
                    with open(os.path.join(out, name), encoding="utf-8") as f:
                        files[name] = f.read()
                except OSError:
                    files[name] = None
            return code, said.getvalue(), files

        in_force = ("semgrep", new3, "2026-09-26", "2027-03-01")

        lay_baseline()
        code, said, files = run([2, 6])
        expect(code == 0, "a run whose findings are all in the baseline passes: exit %d\n%s" % (code, said))
        code, said, files = run([2, 3, 6])
        expect(code == 1 and new3 in said, "a finding new to the baseline fails the job, and is named by its fingerprint: "
                                           "exit %d\n%s" % (code, said))
        code, said, files = run([2, 3, 6], "--exceptions", exceptions(in_force))
        expect(code == 0, "a new finding an exception in force accepts passes: exit %d\n%s" % (code, said))
        expect("src/a.py:3" in (files["accepted-findings.txt"] or "") and not files["new-findings.txt"]
               and "<failure" not in (files["junit.xml"] or "x<failure"),
               "an accepted finding is listed as accepted, not new, and fails no test case: %s" % files)
        code, said, files = run([2, 3, 4, 6], "--exceptions", exceptions(in_force))
        expect(code == 1 and "src/a.py:4" in (files["new-findings.txt"] or "")
               and "src/a.py:3" not in (files["new-findings.txt"] or ""),
               "a new finding no exception accepts still fails the job, beside one that is accepted: exit %d\n%s" % (code, said))
        code, said, files = run([2, 3, 6], "--exceptions", exceptions(("semgrep", new3, "2026-01-05", "2026-09-30")))
        expect(code == 1 and "expired on 2026-09-30" in said, "an exception past its expiry accepts nothing: exit %d\n%s" % (code, said))
        code, said, files = run([2, 3, 6], "--exceptions", exceptions(("static-analysis", new3, "2026-09-26", "2027-03-01")))
        expect(code == 1, "an exception for the other job accepts nothing here: exit %d\n%s" % (code, said))
        code, said, files = run([2, 3, 6], "--exceptions", exceptions(("semgrep", new3, "2026-09-26", "2026-10-20")))
        expect(code == 0 and "WARNING: the exception for semgrep %s" % new3 in said,
               "an exception within %d days of its expiry is named, and still accepts: exit %d\n%s" % (
                   EXPIRY_WARNING_DAYS, code, said))
        malformed = os.path.join(work, "malformed.yml")
        with open(malformed, "w", encoding="utf-8") as f:
            f.write("foo: bar\n")
        code, said, files = run([2, 3, 6], "--exceptions", malformed)
        expect(code == 2 and "not well formed" in said and files["junit.xml"] and "<failure" in files["junit.xml"],
               "an exceptions file that is not well formed fails the job as unjudged: exit %d\n%s" % (code, said))
        code, said, files = run([2, 6], "--exceptions", os.path.join(work, "nowhere.yml"))
        expect(code == 2 and "cannot be read" in said, "an exceptions file that is not there fails the job: exit %d\n%s" % (code, said))
        code, said, files = run([2, 3, 6], "--exceptions", exceptions(("semgrep", new3, "2026-12-01", "2027-01-01")))
        expect(code == 2, "an exception said to be reviewed after today makes the file unjudged: exit %d\n%s" % (code, said))
        code, said, files = run([2, 3, 6], "--exceptions", exceptions(in_force),
                                holes=[{"code": 3, "level": "warn", "type": "Syntax error", "path": "src/b.py",
                                        "message": "Syntax error at line src/b.py:1:"}])
        expect(code == 2, "a hole in the scan fails the job whatever an exception accepts: exit %d\n%s" % (code, said))

        # The rewrite: what is still found stays, at its line now and with its reason; what is
        # gone goes; nothing new comes in, and each finding left out is named.
        reasons = os.path.join(work, "reasons.json")
        with open(reasons, "w", encoding="utf-8") as f:
            json.dump({known6: "Triaged 6 October 2026: a reason given now.", known2: "not the reason it already has",
                       new3: "a reason does not bring a finding in"}, f)
        lay_baseline()
        code, said, files = run([2, 3, 4, 6], "--write-baseline", "--reasons", reasons, "--exceptions", exceptions(in_force))
        written = read_baseline(baseline)
        expect(sorted(written) == sorted([known2, known6]),
               "the rewrite keeps what is still found, drops what is gone and adds nothing: %s" % sorted(written))
        expect(written.get(known2, [""] * 5)[4:] == [old_reason], "a reason stays with its finding: %s" % written.get(known2))
        expect(written.get(known2, [""] * 3)[2] == "src/a.py:2", "a kept entry is written at its line now: %s" % written.get(known2))
        expect(written.get(known6, [""] * 5)[4:] == ["Triaged 6 October 2026: a reason given now."],
               "--reasons gives a reason to a kept entry that had none: %s" % written.get(known6))
        expect(code == 1 and "left out" in said and new3 in said and new4 in said and "dropped" in said and gone in said,
               "the rewrite names what it dropped and what it left out, and the finding nobody accepted fails the job: "
               "exit %d\n%s" % (code, said))
        with open(baseline, encoding="utf-8") as f:
            text = f.read()
        expect("# 2 findings, 2 of them with a reason.\n" in text and text.startswith(BASELINE_HEADER[0] % "semgrep"),
               "the rewritten file says what it holds: %s" % text[:200])
        lay_baseline()
        with open(reasons, "w", encoding="utf-8") as f:
            json.dump({"rule:rule-6": "Triaged 6 October 2026: every rule-6 finding is a constant."}, f)
        code, said, files = run([2, 6], "--write-baseline", "--reasons", reasons)
        expect(code == 0 and read_baseline(baseline).get(known6, [""] * 5)[4:] ==
               ["Triaged 6 October 2026: every rule-6 finding is a constant."],
               "--reasons gives a reason by rule too: exit %d, %s" % (code, read_baseline(baseline).get(known6)))
    finally:
        os.chdir(held_cwd)
        ROOT = held_root
        _top_level = None
        _lines_cache.clear()
        shutil.rmtree(work, ignore_errors=True)

    # The tree's own baselines: one entry a line, each fingerprint once, and the count
    # line saying what the file holds.
    for job, relative_path in sorted(BASELINES.items()):
        path = os.path.join(ROOT, relative_path)
        entries, with_reason, seen, said = 0, 0, set(), None
        with open(path, encoding="utf-8") as f:
            for number, raw in enumerate(f, 1):
                line = raw.rstrip("\r\n")
                count = re.match(r"^# (\d+) findings, (\d+) of them with a reason\.$", line)
                if count:
                    said = (int(count.group(1)), int(count.group(2)))
                if not line or line.startswith("#"):
                    continue
                fields = line.split("\t")
                entries += 1
                expect(re.match(r"^[0-9a-f]{20}$", fields[0]) and len(fields) in (5, 6) and fields[0] not in seen,
                       "%s line %d: fingerprint, tool, rule, file:line, message and a reason or none, the fingerprint "
                       "once: %r" % (relative_path, number, line[:120]))
                seen.add(fields[0])
                with_reason += 1 if len(fields) == 6 and fields[5].strip() else 0
        expect(said == (entries, with_reason), "%s says it holds %s findings and reasons, and holds %d, %d with a reason" % (
            relative_path, said, entries, with_reason))

    for failure in failures:
        print("FAILED: " + failure)
    print("security-findings.py self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
