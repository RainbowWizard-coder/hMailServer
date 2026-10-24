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
(9 MiB) is written as cobertura-1.xml, cobertura-2.xml and so on, beside the --output
name's folder: whole packages to a file where a package fits in one, and a package
larger than a file split between its classes - its source files, each whole in one
file - with the package's own rates in each part, as each file carries the run's
totals. The job collects them with a wildcard, and GitLab adds each class to one
report by its filename. Only a source file whose own class cannot fit in a file is
refused.

An earlier run's report beside --output (the file, and its numbered parts) is
removed before anything is read, and what this run wrote is removed when it fails,
so that no report but this run's whole one is left for the job to collect.

Usage:
  python3 build/ci/coverage-cobertura.py --lcov lcov.info --root DIR --output cobertura.xml [--summary coverage.json]
  python3 build/ci/coverage-cobertura.py --self-test

Exit codes: 0 written, 1 refused (the reason is printed, and no report is left), 2 usage.
"""

import argparse
import contextlib
import io
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


def nbytes(text):
    return len(text.encode("utf-8"))


def by_package(files, root):
    """(every source file by its path from root, with its records, and the folders'
    package_parts, in order): lcov's files, as parse_lcov reads them, all under root."""
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
    by_folder = {}
    for rel in sorted(named):
        folder = rel.rsplit("/", 1)[0] if "/" in rel else "."
        by_folder.setdefault(folder, []).append((rel, named[rel]))
    return named, [package_parts(folder, members) for folder, members in sorted(by_folder.items())]


def package_parts(folder, members):
    """A folder's <package>: its opening tag, with the folder's own rates, each of its
    classes as (file name, XML), and its closing tag. A package split between files
    opens the same way in each."""
    t = totals(dict(members))
    head = '<package name=%s line-rate="%s" branch-rate="%s" complexity="0"><classes>' % (
        quoteattr(folder.replace("/", ".")), rate(*t["lines"]), rate(*t["branches"]))
    return head, [(rel, class_xml(rel, record)) for rel, record in members], "</classes></package>\n"


def documents(packages, t, stamp, max_bytes):
    """The report's text: one document, or, when that is larger than max_bytes, several,
    each of at most max_bytes - whole packages to a document where a package fits in
    one, in order, and a package larger than a document split between its classes,
    each class whole in one document. packages are package_parts's."""
    whole_packages = [head + "".join(xml for _, xml in classes) + tail for head, classes, tail in packages]
    whole = document(whole_packages, t, stamp)
    if nbytes(whole) <= max_bytes:
        return [whole]
    empty = nbytes(document([], t, stamp))
    parts, current, size = [], [], empty
    for (head, classes, tail), package in zip(packages, whole_packages):
        length = nbytes(package)
        if empty + length <= max_bytes:
            # A package that fits in a document goes whole: in this one, or the next.
            if current and size + length > max_bytes:
                parts.append(current)
                current, size = [], empty
            current.append(package)
            size += length
            continue
        # Larger than a document: its classes, in order, from the room left in this
        # one on, the package's opening and closing tags around each part of it.
        frame = nbytes(head) + nbytes(tail)
        chunk, chunk_size = [], frame
        for rel, xml in classes:
            length = nbytes(xml)
            if empty + frame + length > max_bytes:
                raise Refused("%s's class comes to %d bytes, more than a file of at most %d can hold with the "
                              "document's head and tail and its folder's package around it" % (rel, length, max_bytes))
            if size + chunk_size + length > max_bytes:
                if chunk:
                    current.append(head + "".join(chunk) + tail)
                if current:
                    parts.append(current)
                current, size = [], empty
                chunk, chunk_size = [], frame
            chunk.append(xml)
            chunk_size += length
        current.append(head + "".join(chunk) + tail)
        size += chunk_size
    parts.append(current)
    return [document(part, t, stamp) for part in parts]


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
    """Writes the report (one file, or several beside output); returns the paths. An
    earlier run's report beside output goes first, whatever this run then does."""
    remove_report(output)
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
    named, packages = by_package(files, root)
    t = totals(named)
    stamp = int(time.time()) if stamp is None else stamp
    # Each file carries the run's totals at its top, so that any one of them read alone
    # says what the whole measured; GitLab reads only the classes' lines.
    texts = documents(packages, t, stamp, max_bytes)
    stem, ext = os.path.splitext(output)
    written = [output] if len(texts) == 1 else ["%s-%d%s" % (stem, index, ext) for index in range(1, len(texts) + 1)]
    for path, text in zip(written, texts):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    said = "Cobertura report: %d source files in %d folders, %s, each source line once" % (len(named), len(packages), figures(t))
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


def remove_report(output):
    """The report beside output, the file and its numbered parts, removed."""
    for path in [output] + _numbered(output):
        if os.path.isfile(path):
            os.remove(path)


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
# For the split: a folder of three files, larger than the others, and names beyond
# ASCII, whose bytes outnumber their characters. 19 lines, 12 run; 2 branches, 1 run.
SPLIT_SAMPLE = """SF:{root}/hmailserver/source/Server/common/Mime/Mime.cpp
DA:1,1
DA:2,0
DA:3,4
DA:4,0
DA:5,2
DA:6,1
BRDA:3,0,0,1
BRDA:3,0,1,0
end_of_record
SF:{root}/hmailserver/source/Server/common/Mime/Zeichensätze€.cpp
DA:1,0
DA:2,0
DA:3,1
DA:4,1
end_of_record
SF:{root}/hmailserver/source/Server/common/Mime/MimeHeader.cpp
DA:7,1
DA:8,1
DA:9,0
DA:10,3
DA:11,0
end_of_record
SF:{root}/hmailserver/source/Server/SMTP/SMTPConnection.cpp
DA:1,1
DA:2,1
DA:3,0
end_of_record
SF:{root}/hmailserver/source/Server/platform/über.cpp
DA:1,1
end_of_record
"""
# Its classes in the report's order: folders by name, and the files of each by name.
SPLIT_ORDER = ["hmailserver/source/Server/SMTP/SMTPConnection.cpp", "hmailserver/source/Server/common/Mime/Mime.cpp",
               "hmailserver/source/Server/common/Mime/MimeHeader.cpp",
               "hmailserver/source/Server/common/Mime/Zeichensätze€.cpp",
               "hmailserver/source/Server/platform/über.cpp"]


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
                expect(needle in str(e) and not os.path.exists(out) and not _numbered(out),
                       "%s, and nothing is written: %s" % (what, e))
                # The job prints its figure before this runs, and GitLab reads the last
                # line of the log its coverage: expression matches.
                expect("lines: " not in str(e), "%s, in words other than summary.txt's lines figure: %s" % (what, e))

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
        refused("a source file too large for any file is refused", "more than a file", summary=False, max_bytes=300)

        # Every limit from below the smallest a file can be up to the whole report: each
        # file within the limit in bytes, a later one too, so that a file's own head and
        # tail are counted in every one, and with names beyond ASCII, whose bytes the
        # limit counts and not their characters; split when the whole is over the limit
        # and only then; every class once, in order; a folder that fits in a file whole
        # in one, and a larger one in consecutive files, its classes whole; each file
        # well-formed and carrying the run's totals; and below the smallest, refused.
        files = parse_lcov(SPLIT_SAMPLE.format(root=root))
        named, packages = by_package(files, root)
        t = totals(named)
        expect(t == {"lines": (12, 19), "branches": (1, 2)}, "the split sample's totals: %s" % t)
        whole_text = documents(packages, t, 1, 10 ** 9)[0]
        whole = nbytes(whole_text)
        expect(whole > len(whole_text), "the split sample's names are beyond ASCII")
        lowest = max(nbytes(document([head + xml + tail], t, 1)) for head, classes, tail in packages for _, xml in classes)
        alone = {}
        for head, classes, tail in packages:
            name = ET.fromstring(head + tail).get("name")  # nosemgrep: the converter's own tags
            alone[name] = nbytes(document([head + "".join(xml for _, xml in classes) + tail], t, 1))
        expect(max(alone.values()) > lowest, "one folder is larger than the smallest file can be: %s, %d" % (alone, lowest))
        # Each folder's own rates (lines run of its lines; branches), in every file it is in.
        rates = {"hmailserver.source.Server.SMTP": ("0.6667", "1.0000"), "hmailserver.source.Server.common.Mime": ("0.6000", "0.5000"),
                 "hmailserver.source.Server.platform": ("1.0000", "1.0000")}
        splits = 0
        for limit in range(lowest - 50, whole + 2):
            try:
                texts = documents(packages, t, 1, limit)
            except Refused as e:
                expect(limit < lowest and "more than a file" in str(e), "limit %d: refused: %s" % (limit, e))
                continue
            if limit < lowest:
                failures.append("limit %d: a class that fits in no file of it was not refused" % limit)
                continue
            sizes = [nbytes(x) for x in texts]
            expect(max(sizes) <= limit, "limit %d: every file within it, in bytes: %s" % (limit, sizes))
            expect((len(texts) > 1) == (whole > limit), "limit %d: split exactly when the whole, %d bytes, is over it: %d file(s)"
                   % (limit, whole, len(texts)))
            seen, homes = [], {}
            for index, x in enumerate(texts):
                part = ET.fromstring(x.encode("utf-8"))  # nosemgrep: the report this test just made
                expect(part.get("lines-valid") == "19" and part.get("lines-covered") == "12"
                       and part.get("branches-valid") == "2", "limit %d: file %d carries the run's totals" % (limit, index + 1))
                for package in part.iter("package"):
                    homes.setdefault(package.get("name"), []).append(index)
                    seen += [c.get("filename") for c in package.iter("class")]
                    expect((package.get("line-rate"), package.get("branch-rate")) == rates.get(package.get("name")),
                           "limit %d: %s in file %d with its folder's own rates: %s" % (limit, package.get("name"), index + 1,
                                                                                         package.attrib))
            expect(seen == SPLIT_ORDER, "limit %d: every class once, in order: %s" % (limit, seen))
            for name, where in homes.items():
                expect(where == list(range(where[0], where[0] + len(where))),
                       "limit %d: %s in one file or in consecutive ones, once in each: %s" % (limit, name, where))
                expect(len(where) == 1 or alone.get(name, 0) > limit,
                       "limit %d: %s, which fits in a file, is whole in one: %s" % (limit, name, where))
            splits += any(len(where) > 1 for where in homes.values())
        expect(splits > 0, "and at some limit a folder was split between files")

        # Written: numbered, each file within the limit on disk, the large folder in two
        # or more; and a run that fails half-way, here at its second file, leaves
        # nothing of what it wrote for the job to hand GitLab.
        middle = (lowest + max(alone.values())) // 2
        with open(lcov, "w", encoding="utf-8") as f:
            f.write(SPLIT_SAMPLE.format(root=root))
        parts = convert(lcov, root, out, None, max_bytes=middle, stamp=1, say=quiet)
        expect([os.path.basename(p) for p in parts] == ["cobertura-%d.xml" % i for i in range(1, len(parts) + 1)]
               and len(parts) >= 2, "a split report is cobertura-1.xml and on: %s" % parts)
        mime = 0
        for p in parts:
            expect(os.path.getsize(p) <= middle, "%s is within %d bytes on disk (%d)" % (p, middle, os.path.getsize(p)))
            part = ET.parse(p).getroot()  # nosemgrep: the report this test just wrote
            mime += any(k.get("name") == "hmailserver.source.Server.common.Mime" for k in part.iter("package"))
        expect(mime >= 2, "the folder larger than a file is in %d files" % mime)
        for p in parts:
            os.remove(p)
        blocker = os.path.join(work, "cobertura-2.xml")
        os.makedirs(blocker)
        saved, heard = sys.argv, io.StringIO()
        sys.argv = ["coverage-cobertura.py", "--lcov", lcov, "--root", root, "--output", out, "--max-bytes", str(middle)]
        try:
            with contextlib.redirect_stdout(heard):
                code = main()
        finally:
            sys.argv = saved
            os.rmdir(blocker)
        expect(code == 1 and "REFUSED: no Cobertura report" in heard.getvalue(),
               "a write that fails is a refusal: exit %s, %r" % (code, heard.getvalue()))
        expect(not os.path.exists(out) and not _numbered(out), "and what it wrote before it is gone: %s" % _numbered(out))
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
    except (Refused, OSError, ValueError, KeyError, IndexError, TypeError) as e:
        # Nothing half-written is left for the job's artifacts to hand GitLab.
        try:
            remove_report(args.output)
        except OSError as gone:
            print("could not remove what was written beside %s: %s" % (args.output, gone))
        print("REFUSED: no Cobertura report: %s" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
