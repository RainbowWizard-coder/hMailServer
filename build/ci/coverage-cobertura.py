#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The coverage job's lcov export as a Cobertura report, for GitLab.

build/ci/linux-coverage.sh writes llvm-cov's own exports: coverage.json (the totals)
and lcov.info (every line's and every branch's count). GitLab reads neither. Its
artifacts:reports:coverage_report takes Cobertura XML (or JaCoCo), and from it marks
each changed line of a merge request's diff as run or not, when that merge request's
head pipeline ran the job; it reads each <class>'s filename and each <line>'s number
and hits, nothing else (Gitlab::Ci::Parsers::Coverage::Documents::CoberturaDocument).
The percentage GitLab shows is another matter: the job's coverage: regular expression
reads it from the log, from summary.txt's lines figure.

This writes that report from lcov.info:

  * one <class> per source file, its filename the path relative to the checkout's
    root (--root), which is what GitLab matches against the merge request's files -
    no <sources>, since GitLab drops a class it cannot find under a source it reads;
  * one <line> per source line llvm-cov counted (lcov's DA records), with its count,
    and on a line with branches branch="true" and condition-coverage, the branches
    run of those there (its BRDA records);
  * one <package> per folder, and the line and branch rates of each level.

Two counts of one run. llvm-cov's summary - coverage.json, summary.txt and so the
figure GitLab shows - adds up each function's lines and branches, so that a line
inside a lambda counts once in the lambda and once in the function around it: on
24 October 2026 it counted 307,733 lines where the source has 300,380 that llvm-cov
maps, 2,043 of the difference in one tester whose checks are lambdas. The report's
<line> elements are the source's lines, each once, and its totals and rates are
theirs. llvm-cov writes its summary's figures into lcov.info as well, each file's
LF, LH, BRF and BRH; with --summary (coverage.json) those must add up to the
summary's totals and the two must name the same files, or nothing is written and
this exits 1: the report and the figure are one export of one run, or there is no
report. A source file outside --root is not GitLab's to match and is refused the
same way, as is an lcov.info with no file in it.

GitLab reads at most 10 MiB of a Cobertura file. A report larger than --max-bytes
(9 MiB) is written as cobertura-1.xml, cobertura-2.xml and so on, whole packages to a
file, beside the --output name's folder; the job collects them with a wildcard and
GitLab merges them into one.

Usage:
  python3 build/ci/coverage-cobertura.py --lcov lcov.info --root DIR --output cobertura.xml [--summary coverage.json]
  python3 build/ci/coverage-cobertura.py --self-test

Exit codes: 0 written, 1 refused (the reason is printed), 2 usage.
"""

import argparse
import json
import os
import shutil
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from xml.sax.saxutils import quoteattr

MAX_BYTES = 9 * 1024 * 1024
DOCTYPE = '<!DOCTYPE coverage SYSTEM "http://cobertura.sourceforge.net/xml/coverage-04.dtd">'
# The per-file figures of llvm-cov's own summary that it writes into each record.
DECLARED = ("LF", "LH", "BRF", "BRH")


class Refused(Exception):
    pass


def parse_lcov(text):
    """{path: {"lines": {number: hits}, "branches": {number: [taken, ...]},
    "declared": {"LF": n, ...}}} from lcov text. A branch never reached is "-" in lcov,
    and 0 taken here. A file named twice has its counts added, line by line."""
    files = {}
    current = None
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        key, _, value = line.partition(":")
        if key == "SF":
            if current is not None:
                raise Refused("lcov line %d: SF before the previous file's end_of_record" % number)
            if not value:
                raise Refused("lcov line %d: an SF with no path" % number)
            current = files.setdefault(value, {"lines": {}, "branches": {}, "declared": {}})
        elif line == "end_of_record":
            if current is None:
                raise Refused("lcov line %d: end_of_record with no file" % number)
            current = None
        elif key in ("DA", "BRDA") + DECLARED:
            if current is None:
                raise Refused("lcov line %d: %s outside a file's record" % (number, key))
            parts = value.split(",")
            try:
                if key == "DA":
                    at, hits = int(parts[0]), int(parts[1])
                    if at < 1 or hits < 0:
                        raise ValueError
                    current["lines"][at] = current["lines"].get(at, 0) + hits
                elif key == "BRDA":
                    if len(parts) != 4:
                        raise ValueError
                    at = int(parts[0])
                    taken = 0 if parts[3] == "-" else int(parts[3])
                    if at < 1 or taken < 0:
                        raise ValueError
                    current["branches"].setdefault(at, []).append(taken)
                else:
                    count = int(value)
                    if count < 0:
                        raise ValueError
                    current["declared"][key] = current["declared"].get(key, 0) + count
            except (ValueError, IndexError):
                raise Refused("lcov line %d: %r is not a %s record" % (number, line[:120], key))
    if current is not None:
        raise Refused("lcov: the last file has no end_of_record - the export was cut short")
    if not files:
        raise Refused("lcov: no source file in it")
    return files


def relative(path, root):
    """path relative to root, with forward slashes, or None when it is not under it."""
    root = os.path.normpath(root)
    full = os.path.normpath(path if os.path.isabs(path) else os.path.join(root, path))
    try:
        rel = os.path.relpath(full, root)
    except ValueError:
        return None
    rel = rel.replace(os.sep, "/")
    if rel == "." or rel.startswith("../") or rel == "..":
        return None
    return rel


def totals(files):
    """The source's lines and branches, each once: (run, counted) of each."""
    lines = sum(len(f["lines"]) for f in files.values())
    lines_run = sum(1 for f in files.values() for hits in f["lines"].values() if hits > 0)
    branches = sum(len(b) for f in files.values() for b in f["branches"].values())
    branches_run = sum(1 for f in files.values() for b in f["branches"].values() for taken in b if taken > 0)
    return {"lines": (lines_run, lines), "branches": (branches_run, branches)}


def declared_totals(files):
    """llvm-cov's own summary figures, as lcov.info carries them: (run, counted) of each."""
    missing = sorted(p for p, f in files.items() if any(k not in f["declared"] for k in DECLARED))
    if missing:
        raise Refused("%d file(s) of lcov.info carry no LF, LH, BRF and BRH, llvm-cov's own figures, to hold to the "
                      "summary: %s" % (len(missing), ", ".join(missing[:5])))
    add = lambda key: sum(f["declared"][key] for f in files.values())
    return {"lines": (add("LH"), add("LF")), "branches": (add("BRH"), add("BRF"))}


def rate(run, count):
    # A level with nothing to count is wholly covered, as Cobertura's own writers give it.
    return "%.4f" % (run / count if count else 1.0)


def read_summary(path):
    """(totals, the files it names) of llvm-cov export -summary-only's file."""
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    t = data["data"][0]["totals"]
    names = {entry["filename"] for entry in data["data"][0]["files"]}
    return ({"lines": (t["lines"]["covered"], t["lines"]["count"]),
             "branches": (t["branches"]["covered"], t["branches"]["count"])}, names)


def class_xml(rel, record):
    t = totals({rel: record})
    out = ['<class name=%s filename=%s line-rate="%s" branch-rate="%s" complexity="0"><methods/><lines>' % (
        quoteattr(rel.rsplit("/", 1)[-1]), quoteattr(rel), rate(*t["lines"]), rate(*t["branches"]))]
    for at in sorted(record["lines"]):
        hits = record["lines"][at]
        taken = record["branches"].get(at)
        if taken:
            reached = sum(1 for t in taken if t > 0)
            out.append('<line number="%d" hits="%d" branch="true" condition-coverage="%d%% (%d/%d)"/>' % (
                at, hits, reached * 100 // len(taken), reached, len(taken)))
        else:
            out.append('<line number="%d" hits="%d"/>' % (at, hits))
    out.append("</lines></class>")
    return "".join(out)


def package_xml(name, members):
    t = totals(dict(members))
    body = "".join(class_xml(rel, record) for rel, record in members)
    return '<package name=%s line-rate="%s" branch-rate="%s" complexity="0"><classes>%s</classes></package>\n' % (
        quoteattr(name), rate(*t["lines"]), rate(*t["branches"]), body)


def document(packages, t, stamp):
    head = ('<?xml version="1.0" encoding="UTF-8"?>\n%s\n'
            '<coverage line-rate="%s" branch-rate="%s" lines-covered="%d" lines-valid="%d" branches-covered="%d" '
            'branches-valid="%d" complexity="0" version="hmailserver-coverage-cobertura 1" timestamp="%d">\n<packages>\n') % (
        DOCTYPE, rate(*t["lines"]), rate(*t["branches"]), t["lines"][0], t["lines"][1], t["branches"][0], t["branches"][1], stamp)
    return head + "".join(packages) + "</packages>\n</coverage>\n"


def figures(t):
    return "%s of %s lines run and %s of %s branches" % (
        format(t["lines"][0], ","), format(t["lines"][1], ","), format(t["branches"][0], ","), format(t["branches"][1], ","))


def convert(lcov_path, root, output, summary=None, max_bytes=MAX_BYTES, stamp=None, say=print):
    """Writes the report (one file, or several beside output); returns the paths."""
    with open(lcov_path, encoding="utf-8", errors="strict") as f:
        files = parse_lcov(f.read())
    outside = sorted(p for p in files if relative(p, root) is None)
    if outside:
        raise Refused("%d source file(s) outside %s, which GitLab cannot match to the repository: %s" % (
            len(outside), root, ", ".join(outside[:5])))
    own = None
    if summary:
        expected, names = read_summary(summary)
        only_lcov, only_summary = sorted(set(files) - names), sorted(names - set(files))
        if only_lcov or only_summary:
            raise Refused("lcov.info and the summary (%s) do not name the same files: %d only in lcov.info (%s), %d only in "
                          "the summary (%s)" % (summary, len(only_lcov), ", ".join(only_lcov[:3]), len(only_summary),
                                                ", ".join(only_summary[:3])))
        own = declared_totals(files)
        if own != expected:
            raise Refused("lcov.info's own figures are not the summary's (%s): %s against %s" % (
                summary, figures(own), figures(expected)))
    named = {}
    for path, record in files.items():
        rel = relative(path, root)
        if rel in named:
            # Two spellings of one file (a/./b and a/b): one class, the counts added.
            for at, hits in record["lines"].items():
                named[rel]["lines"][at] = named[rel]["lines"].get(at, 0) + hits
            for at, taken in record["branches"].items():
                named[rel]["branches"].setdefault(at, []).extend(taken)
        else:
            named[rel] = record
    t = totals(named)
    stamp = int(time.time()) if stamp is None else stamp
    by_folder = {}
    for rel in sorted(named):
        folder = rel.rsplit("/", 1)[0] if "/" in rel else "."
        by_folder.setdefault(folder, []).append((rel, named[rel]))
    packages = [package_xml(folder.replace("/", "."), members) for folder, members in sorted(by_folder.items())]
    whole = document(packages, t, stamp)
    for old in _numbered(output):
        os.remove(old)
    if len(whole.encode("utf-8")) <= max_bytes:
        written = [output]
        with open(output, "w", encoding="utf-8", newline="\n") as f:
            f.write(whole)
    else:
        if os.path.exists(output):
            os.remove(output)
        empty = len(document([], t, stamp).encode("utf-8"))
        groups, current, size = [], [], empty
        for package in packages:
            length = len(package.encode("utf-8"))
            if empty + length > max_bytes:
                raise Refused("one folder's classes come to %d bytes, more than the %d a file may hold" % (length, max_bytes))
            if current and size + length > max_bytes:
                groups.append(current)
                current, size = [], empty
            current.append(package)
            size += length
        groups.append(current)
        stem, ext = os.path.splitext(output)
        written = []
        for index, group in enumerate(groups, 1):
            # Each file carries the run's totals at its top, so that any one of them
            # read alone says what the whole measured; GitLab reads only the lines.
            path = "%s-%d%s" % (stem, index, ext)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(document(group, t, stamp))
            written.append(path)
    said = "Cobertura report: %d source files in %d folders, %s, each source line once" % (len(named), len(by_folder), figures(t))
    if own:
        said += "; llvm-cov's own count, the summary's, is %s, a line inside a lambda counted in it and in the function " \
                "around it" % figures(own)
    say(said + "; in %s" % ", ".join(os.path.basename(p) for p in written))
    return written


def _numbered(output):
    stem, ext = os.path.splitext(output)
    folder = os.path.dirname(output) or "."
    base = os.path.basename(stem)
    found = []
    if os.path.isdir(folder):
        for name in os.listdir(folder):
            middle = name[len(base) + 1:-len(ext)] if ext else name[len(base) + 1:]
            if name.startswith(base + "-") and name.endswith(ext) and middle.isdigit():
                found.append(os.path.join(folder, name))
    return found


# ---------------------------------------------------------------- the self-test

# Account.cpp's LF and LH say 5 and 4 where its DA records are 4 lines, 3 run: one
# line counted twice by llvm-cov's summary, as a lambda's line is.
SAMPLE = """TN:
SF:{root}/hmailserver/source/Server/common/BO/Account.cpp
FN:10,_ZN2HM7Account4SaveEv
FNDA:3,_ZN2HM7Account4SaveEv
FNF:1
FNH:1
DA:10,3
DA:11,3
DA:12,0
DA:14,2
BRDA:11,0,0,2
BRDA:11,0,1,1
BRDA:14,0,0,-
BRDA:14,0,1,-
BRF:4
BRH:2
LF:5
LH:4
end_of_record
SF:{root}/hmailserver/source/Server/common/Util/A&B<x>.cpp
DA:1,0
DA:2,0
BRF:0
BRH:0
LF:2
LH:0
end_of_record
SF:{root}/hmailserver/source/Server/platform/main.cpp
DA:5,1
BRF:0
BRH:0
LF:1
LH:1
end_of_record
"""
NAMES = ["hmailserver/source/Server/common/BO/Account.cpp", "hmailserver/source/Server/common/Util/A&B<x>.cpp",
         "hmailserver/source/Server/platform/main.cpp"]


def self_test():
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    quiet = lambda line: None
    work = tempfile.mkdtemp(prefix="coverage-cobertura-")
    try:
        root = os.path.join(work, "checkout")
        os.makedirs(root)
        lcov = os.path.join(work, "lcov.info")
        with open(lcov, "w", encoding="utf-8") as f:
            f.write(SAMPLE.format(root=root))
        summary = os.path.join(work, "coverage.json")

        def write_summary(lines, branches, names):
            # The files by the names the lcov records give them, as linux-coverage.sh's
            # sed leaves both exports.
            with open(summary, "w", encoding="utf-8") as f:
                json.dump({"data": [{"totals": {"lines": {"count": lines[1], "covered": lines[0]},
                                                "branches": {"count": branches[1], "covered": branches[0]}},
                                     "files": [{"filename": root + "/" + n} for n in names]}]}, f)

        out = os.path.join(work, "cobertura.xml")

        def refused(what, needle, **extra):
            try:
                convert(lcov, root, out, summary if extra.get("summary", True) else None, stamp=1, say=quiet,
                        **{k: v for k, v in extra.items() if k != "summary"})
                failures.append(what)
            except Refused as e:
                expect(needle in str(e) and not os.path.exists(out), "%s, and nothing is written: %s" % (what, e))

        write_summary((5, 8), (2, 4), NAMES)
        said = []
        written = convert(lcov, root, out, summary, stamp=1, say=said.append)
        expect(written == [out], "a small report is one file: %s" % written)
        tree = ET.parse(out).getroot()  # nosemgrep: the report this test just wrote
        expect(tree.tag == "coverage" and tree.get("lines-covered") == "4" and tree.get("lines-valid") == "7"
               and tree.get("branches-covered") == "2" and tree.get("branches-valid") == "4",
               "the totals are the source's lines, each once (DA), not llvm-cov's own (LF): %s" % tree.attrib)
        expect(tree.get("line-rate") == "0.5714" and tree.get("branch-rate") == "0.5000", "and its rates: %s" % tree.attrib)
        expect(tree.find("sources") is None, "no <sources>: GitLab would look for every file under one")
        classes = {c.get("filename"): c for c in tree.iter("class")}
        expect(sorted(classes) == sorted(NAMES), "each file by its path from the checkout's root, as GitLab matches it: %s"
               % sorted(classes))
        account = classes.get(NAMES[0])
        lines = {int(l.get("number")): l for l in account.iter("line")} if account is not None else {}
        expect(sorted(lines) == [10, 11, 12, 14] and lines[10].get("hits") == "3" and lines[12].get("hits") == "0",
               "every counted line, with its count")
        expect(lines.get(11) is not None and lines[11].get("branch") == "true" and lines[11].get("condition-coverage") == "100% (2/2)",
               "a line whose two branches both ran: 100% (2/2)")
        expect(lines.get(14) is not None and lines[14].get("condition-coverage") == "0% (0/2)", "a branch never reached, '-', is 0 taken")
        expect(lines.get(10) is not None and lines[10].get("branch") is None, "a line with no branch says none")
        packages = [p.get("name") for p in tree.iter("package")]
        expect(packages == ["hmailserver.source.Server.common.BO", "hmailserver.source.Server.common.Util",
                            "hmailserver.source.Server.platform"], "one package per folder, in order: %s" % packages)
        with open(out, encoding="utf-8") as f:
            text = f.read()
        expect("A&amp;B&lt;x&gt;.cpp" in text, "a file name's & and < are escaped")
        expect(any("4 of 7 lines run and 2 of 4 branches, each source line once" in s and "is 5 of 8 lines run" in s for s in said),
               "it says what it wrote, and llvm-cov's own count beside it: %s" % said)
        expect(not any("lines: " in s for s in said),
               "and never in the words of summary.txt's lines figure, which the job's coverage: expression reads: %s" % said)

        # The figure the job reports and the report must be one export of one run.
        os.remove(out)
        write_summary((6, 8), (2, 4), NAMES)
        refused("figures that are not the summary's are refused", "not the summary's")
        write_summary((5, 8), (2, 5), NAMES)
        refused("branches that are not the summary's are refused", "not the summary's")
        write_summary((4, 7), (2, 4), NAMES)
        refused("the source's lines, each once, are not what the summary is held to", "not the summary's")
        write_summary((5, 8), (2, 4), NAMES + ["hmailserver/source/Server/common/BO/Domain.cpp"])
        refused("a summary of another file too is refused", "do not name the same files")
        write_summary((5, 8), (2, 4), NAMES[:2])
        refused("a summary that leaves a file out is refused", "do not name the same files")
        write_summary((5, 8), (2, 4), NAMES)
        expect(convert(lcov, root, out, summary, stamp=1, say=quiet) == [out], "and the summary put right passes again")
        os.remove(out)
        with open(lcov, "w", encoding="utf-8") as f:
            f.write(SAMPLE.format(root=root).replace("LF:1\n", ""))
        refused("an lcov.info without llvm-cov's own figures cannot be held to the summary", "carry no LF")
        expect(convert(lcov, root, out, None, stamp=1, say=quiet) == [out], "though without --summary it is a report")
        os.remove(out)

        # A file outside the root GitLab cannot match: refused, not dropped.
        with open(lcov, "w", encoding="utf-8") as f:
            f.write(SAMPLE.format(root=root) + "SF:/usr/include/c++/14/bits/basic_string.h\nDA:1,1\nend_of_record\n")
        refused("a source file outside the root is refused", "basic_string.h", summary=False)

        for broken, why in [("", "no source file"), ("SF:%s/a.cpp\nDA:1,1\n" % root, "no end_of_record"),
                            ("SF:%s/a.cpp\nDA:x,1\nend_of_record\n" % root, "not a DA record"),
                            ("SF:%s/a.cpp\nBRDA:1,0,0\nend_of_record\n" % root, "not a BRDA record"),
                            ("SF:%s/a.cpp\nLF:-1\nend_of_record\n" % root, "not a LF record"),
                            ("DA:1,1\n", "outside a file's record")]:
            with open(lcov, "w", encoding="utf-8") as f:
                f.write(broken)
            refused("an lcov.info with %s is refused" % why, why, summary=False)

        # Too large for one file: whole packages to a file, each well-formed, and
        # together every class once; the single file of an earlier run goes.
        with open(lcov, "w", encoding="utf-8") as f:
            f.write(SAMPLE.format(root=root))
        with open(out, "w", encoding="utf-8") as f:
            f.write("an earlier run's report")
        stale = os.path.join(work, "cobertura-9.xml")
        with open(stale, "w", encoding="utf-8") as f:
            f.write("an earlier run's part")
        parts = convert(lcov, root, out, None, max_bytes=1100, stamp=1, say=quiet)
        expect(len(parts) >= 2 and all(os.path.basename(p).startswith("cobertura-") for p in parts),
               "a report over the limit is split: %s" % parts)
        expect(not os.path.exists(out) and not os.path.exists(stale), "and an earlier run's files are gone")
        seen = []
        for p in parts:
            expect(os.path.getsize(p) <= 1100, "%s is within the limit (%d bytes)" % (p, os.path.getsize(p)))
            part = ET.parse(p).getroot()  # nosemgrep: the report this test just wrote
            expect(part.get("lines-valid") == "7", "each part carries the run's totals")
            seen += [c.get("filename") for c in part.iter("class")]
        expect(sorted(seen) == sorted(NAMES), "and together every class once: %s" % seen)
        for p in parts:
            os.remove(p)
        refused("a folder too large for any file is refused", "more than", summary=False, max_bytes=300)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--lcov", help="llvm-cov export -format=lcov's file (lcov.info)")
    parser.add_argument("--root", help="the checkout's root, which every file name is made relative to")
    parser.add_argument("--output", help="the report (cobertura.xml); cobertura-1.xml and on when it is split")
    parser.add_argument("--summary", help="llvm-cov export -summary-only's file (coverage.json), whose totals and files "
                                          "lcov.info's own figures must be")
    parser.add_argument("--max-bytes", type=int, default=MAX_BYTES, help="the most a file may hold (9 MiB)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not (args.lcov and args.root and args.output):
        parser.print_usage()
        return 2
    try:
        convert(args.lcov, args.root, args.output, args.summary, args.max_bytes)
    except (Refused, OSError, ValueError, KeyError) as e:
        print("REFUSED: no Cobertura report: %s" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
