#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The original hMailServer's files in REUSE.toml, made from the original's tags.

The project is a fork of Martin Knafve's hMailServer. In September 2026 the
fork's header replaced his notice in the files that came from the original (the
owner's decision, about the files' text), so a file's own header no longer says
that he is a holder of it, and reuse would record the fork alone. This keeps
the record true: it names Martin Knafve as a holder of every file of the
original, as debian/copyright's "Files: *" does, beside what the file says
itself, from the original's own trees rather than from a pattern of
directories, which would credit him with what the fork adds there.

The original's files are the paths that its tags hold -

  hMailServer-5.7.0-b2788, hMailServer-5.7.0-b2843 and hMailServer-5.7.0-b3013

- and those that the fork's first commit, fe45f43c3, took over from the
original's history before them (it was made from an earlier tree of the
original, whose translation tools and licence notices the tags hold at other
paths): build/reuse-original-paths.txt lists them, one a line. A file of the
tree is the original's when its path is one of them, letter case aside (the
fork renamed CreateTablesMYSQL.sql and IMAPCommandLSUB.CPP to their case).

From that list and the tree this writes the tables between the two marker
lines of REUSE.toml: the original's files that carry the project's header,
which names the maintainers, and those that carry none, each with Martin Knafve
as a holder (precedence "aggregate", so that what a file says itself stands
beside it). The third parties' files the original carried are left to their own
tables (THIRD_PARTY below says which, and why), and a table further down
REUSE.toml that names one of the original's files takes the place of these for
it, so it has to name him too - which --holders holds it to.

Usage:
  python3 build/generate-reuse-original.py             write the list (from the
                                                       tags, when the clone has
                                                       them) and the tables
  python3 build/generate-reuse-original.py --check     exit 1 when the list or
                                                       the tables are not what
                                                       this writes; a clone
                                                       without the tags (CI's
                                                       depth-1 one) checks the
                                                       tables against the list
  python3 build/generate-reuse-original.py --holders LINT.json
                                                       exit 1 when reuse lint
                                                       --json's record of one of
                                                       the original's files, but
                                                       a third party's, names no
                                                       Martin Knafve
  python3 build/generate-reuse-original.py --self-test the checks above, against
                                                       records made to break them

build/ci/reuse-lint.sh, the reuse job's script, runs --check and --holders, and
a negative control of each. Every output is written with LF line endings and
compared with CRLF folded to LF, since the tree checks text out with CRLF.
"""

import io
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATHS_FILE = "build/reuse-original-paths.txt"
REUSE_TOML = "REUSE.toml"
TAGS = ("hMailServer-5.7.0-b2788", "hMailServer-5.7.0-b2843", "hMailServer-5.7.0-b3013")
# The fork's first commit, made from an earlier tree of the original.
TAKEN_OVER = "fe45f43c3db6c40dfdb42c57f3db2cc312d53af1"
KNAFVE = "2005-2026 Martin Knafve / hMailServer.com"
FORK = "2026 Christopher Holloway / Progressive Robot Ltd"
LICENCE = "AGPL-3.0-or-later"
BEGIN = "# >>> build/generate-reuse-original.py writes what follows, to the line that ends it."
END = "# <<< the end of what build/generate-reuse-original.py writes."

# The third parties' files that the original carried, which are not Martin
# Knafve's and which their own tables of REUSE.toml give their holders: REUSE.toml
# path patterns ("*" any name, "**" any path), each with the reason. Each must
# still match one of the original's files, so that the list cannot outgrow them.
THIRD_PARTY = (
    ("libraries/fat_cow_icons/**", "the Fat Cow icons, FatCow's"),
    ("hmailserver/source/Tools/DBSetup/database_add.ico", "a Fat Cow icon, byte for byte"),
    ("hmailserver/source/Tools/DBUpdater/database_refresh.ico", "a Fat Cow icon, byte for byte"),
    ("hmailserver/source/Tools/DataDirectorySynchronizer/georectify.ico", "a Fat Cow icon, byte for byte"),
    ("libraries/mssqlce3.5/*.h", "SQL Server Compact's headers, Microsoft's"),
    ("libraries/License - *.rtf", "third parties' licence texts"),
    ("hmailserver/docs/Licenses/License - *", "third parties' licence texts"),
    ("hmailserver/installation/Extras/7za.exe.license.txt", "7-Zip's licence text"),
    ("hmailserver/test/TestData/DKIM/Neutral - Missing bodyhash/*", "Mail::DKIM's test corpus"),
    ("hmailserver/test/TestData/DKIM/PermFail/*", "Mail::DKIM's test corpus, and Port25's reply"),
    ("hmailserver/test/TestData/DKIM/Unsupported/*", "Mail::DKIM's test corpus"),
    ("hmailserver/test/TestData/DKIM/Good/altn.response.txt", "Alt-N's reply"),
    ("hmailserver/test/TestData/DKIM/Good/elandit.response.txt", "Eland Systems' reply"),
    ("hmailserver/test/RegressionTests/Resources/SecuniaBody1.txt", "Secunia's advisory"),
    ("hmailserver/test/SSL examples/google.cer", "the Google Internet Authority's certificate"),
)
# The original's files that reuse does not read: a file named LICENSE is a licence
# text to the REUSE Specification, not a file it needs information for; and reuse
# passes over an empty file (empty() below) as well.
NOT_READ = ("LICENSE",)

# REUSE-IgnoreStart
HEADER_COPYRIGHT = re.compile(r"Copyright \(c\) 2026 Christopher Holloway / Progressive Robot Ltd")
HEADER_LICENCE = re.compile(r"SPDX-License-Identifier:\s*\S")
# REUSE-IgnoreEnd
HEADER_LINES = 25


def glob_regex(pattern):
    """A REUSE.toml path pattern as a regular expression: "**" any path, "*" any
    name within one directory, everything else literal."""
    out = []
    i = 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("^" + "".join(out) + "$")


THIRD_PARTY_RE = [(glob_regex(p), p, why) for p, why in THIRD_PARTY]


def third_party(path):
    for regex, pattern, why in THIRD_PARTY_RE:
        if regex.match(path):
            return pattern
    return None


def git(*args):
    return subprocess.run(["git", "-c", "core.quotepath=off"] + list(args), cwd=ROOT,
                          capture_output=True, check=True).stdout


def git_ok(*args):
    return subprocess.run(["git"] + list(args), cwd=ROOT, capture_output=True).returncode == 0


def tree_paths():
    return sorted(p.decode("utf-8") for p in git("ls-files", "-z").split(b"\0") if p)


def tags_present():
    return all(git_ok("rev-parse", "--verify", "--quiet", ref + "^{commit}") for ref in TAGS + (TAKEN_OVER,))


def paths_at(ref):
    return set(p.decode("utf-8") for p in git("ls-tree", "-r", "-z", "--name-only", ref).split(b"\0") if p)


def originals_from_tags():
    """The paths the three tags hold, and those the fork's first commit took over
    from the original's history before them."""
    originals = set()
    for tag in TAGS:
        originals |= paths_at(tag)
    for path in sorted(paths_at(TAKEN_OVER) - originals):
        if git("log", "-1", "--format=%H", TAGS[-1], "--", path).strip():
            originals.add(path)
    return sorted(originals)


def render_paths(originals):
    lines = [
        "# https://www.progressiverobot.com",
        # REUSE-IgnoreStart
        "# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd",
        "# SPDX-License-Identifier: AGPL-3.0-or-later",
        # REUSE-IgnoreEnd
        "#",
        "# Generated by build/generate-reuse-original.py - do not edit; run it again.",
        "# The original hMailServer's files: every path that its tags",
        "# " + ", ".join(TAGS[:-1]) + " and " + TAGS[-1],
        "# hold, and those that the fork's first commit, " + TAKEN_OVER[:9] + ", took over from",
        "# the original's history before them. One path a line.",
    ]
    for path in originals:
        if path.startswith("#") or "\n" in path:
            sys.exit("%s: a path this list cannot hold" % path)
        lines.append(path)
    return "\n".join(lines) + "\n"


def read_text(relative):
    path = os.path.join(ROOT, relative)
    if not os.path.exists(path):
        return None
    with io.open(path, "r", encoding="utf-8", newline="") as f:
        return f.read().replace("\r\n", "\n")


def read_paths(text):
    return [line for line in text.split("\n") if line and not line.startswith("#")]


def empty(relative):
    """reuse reads no file of no bytes (its covered_files)."""
    path = os.path.join(ROOT, relative)
    return os.path.isfile(path) and os.path.getsize(path) == 0


def original_files(originals, tree):
    """The tree's files that are the original's, at their path or at it letter
    case aside, but for the third parties' files and those reuse does not read:
    (files, third parties' files)."""
    exact = set(originals)
    folded = set(p.lower() for p in originals)
    mine, theirs = [], []
    for path in tree:
        if path in NOT_READ or empty(path):
            continue
        if path in exact or path.lower() in folded:
            (theirs if third_party(path) else mine).append(path)
    return mine, theirs


def carries_header(relative):
    try:
        with open(os.path.join(ROOT, relative), "rb") as f:
            head = f.read(8192).decode("latin-1")
    except OSError:
        return False
    head = "\n".join(head.splitlines()[:HEADER_LINES])
    return bool(HEADER_COPYRIGHT.search(head) and HEADER_LICENCE.search(head))


def toml_string(value):
    if "*" in value or "\\" in value or "\n" in value:
        sys.exit("%s: a path REUSE.toml would read as a pattern" % value)
    return '"' + value.replace('"', '\\"') + '"'


def render_tables(mine):
    header = set(p for p in mine if carries_header(p))
    with_header = [p for p in mine if p in header]
    without = [p for p in mine if p not in header]
    out = [
        BEGIN,
        "# The original hMailServer's files, from build/reuse-original-paths.txt: every file",
        "# of the tree at a path the original's tags hMailServer-5.7.0-b2788, -b2843 and",
        "# -b3013 hold, or that the fork's first commit, fe45f43c3, took over from the",
        "# original's history before them, letter case aside - but for the third parties'",
        "# files among them, which their own tables below give their holders. Martin Knafve",
        "# is a holder of each (from 2005, the year of the original's earliest notices, to",
        "# 2026, that of 5.7.0), beside what the file says itself (precedence aggregate):",
        "# the first table is the files that carry the project's header, which names the",
        "# maintainers, and the second those that carry none, which names the maintainers",
        "# beside him, as debian/copyright's \"Files: *\" does. A table further down that",
        "# names one of these files takes the place of these for it, and names him too;",
        "# build/ci/reuse-lint.sh refuses a record of one of them without him.",
        "[[annotations]]",
        "path = [",
    ]
    out += ["    %s," % toml_string(p) for p in with_header]
    out += [
        "]",
        'precedence = "aggregate"',
        'SPDX-FileCopyrightText = "%s"' % KNAFVE,
        'SPDX-License-Identifier = "%s"' % LICENCE,
        "",
        "[[annotations]]",
        "path = [",
    ]
    out += ["    %s," % toml_string(p) for p in without]
    out += [
        "]",
        'precedence = "aggregate"',
        "SPDX-FileCopyrightText = [",
        '    "%s",' % KNAFVE,
        '    "%s",' % FORK,
        "]",
        'SPDX-License-Identifier = "%s"' % LICENCE,
        END,
    ]
    return "\n".join(out), len(with_header), len(without)


def splice(toml, block):
    start = toml.find(BEGIN + "\n")
    end = toml.find(END + "\n")
    if start < 0 or end < start:
        return None
    return toml[:start] + block + "\n" + toml[end + len(END) + 1:]


def stale_third_parties(theirs):
    used = set(third_party(p) for p in theirs)
    return [p for p, why in THIRD_PARTY if p not in used]


def holders(originals, tree, lint):
    """What is wrong with reuse lint --json's record of the original's files."""
    mine, theirs = original_files(originals, tree)
    record = dict((f["path"], f) for f in lint.get("files", []))
    problems = []
    for path in mine:
        entry = record.get(path)
        if entry is None:
            problems.append("%s: reuse lint did not read it" % path)
        elif not any("Martin Knafve" in c.get("value", "") for c in entry.get("copyrights", [])):
            names = "; ".join(c.get("value", "") for c in entry.get("copyrights", [])) or "nobody"
            problems.append("%s: the original's, and reuse records only %s" % (path, names))
    return mine, theirs, problems


def generate(check):
    status = 0
    have_tags = tags_present()
    committed = read_text(PATHS_FILE)
    if have_tags:
        text = render_paths(originals_from_tags())
        if committed != text:
            if check:
                print("%s is not what the original's tags give; run build/generate-reuse-original.py" % PATHS_FILE)
                status = 1
            else:
                with io.open(os.path.join(ROOT, PATHS_FILE), "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
                print("wrote " + PATHS_FILE)
        committed = text
    elif committed is None:
        print("%s is missing, and this clone has not the original's tags to make it" % PATHS_FILE)
        return 2
    else:
        print("This clone has not the original's tags (%s): %s is taken as committed."
              % (", ".join(TAGS), PATHS_FILE))
    originals = read_paths(committed)
    mine, theirs = original_files(originals, tree_paths())
    stale = stale_third_parties(theirs)
    for pattern in stale:
        print("THIRD_PARTY's %s matches none of the original's files: take it out" % pattern)
        status = 1
    block, with_header, without = render_tables(mine)
    toml = read_text(REUSE_TOML)
    if toml is None:
        print("There is no REUSE.toml")
        return 2
    spliced = splice(toml, block)
    if spliced is None:
        print("REUSE.toml has not the two lines between which this writes:\n  %s\n  %s" % (BEGIN, END))
        return 2
    if spliced != toml:
        if check:
            print("REUSE.toml's tables of the original's files are not what this writes; "
                  "run build/generate-reuse-original.py")
            status = 1
        else:
            with io.open(os.path.join(ROOT, REUSE_TOML), "w", encoding="utf-8", newline="\n") as f:
                f.write(spliced)
            print("wrote " + REUSE_TOML)
    if status == 0:
        print("REUSE.toml names Martin Knafve for the original's %d files (%d with the project's header, "
              "%d without), and leaves its %d files of third parties to their tables"
              % (len(mine), with_header, without, len(theirs)))
    return status


def check_holders(lint_path):
    committed = read_text(PATHS_FILE)
    if committed is None:
        print("%s is missing" % PATHS_FILE)
        return 2
    try:
        with io.open(lint_path, "r", encoding="utf-8") as f:
            lint = json.load(f)
    except (OSError, ValueError) as e:
        print("%s is not reuse lint --json's output: %s" % (lint_path, e))
        return 2
    mine, theirs, problems = holders(read_paths(committed), tree_paths(), lint)
    for problem in problems[:40]:
        print(problem)
    if len(problems) > 40:
        print("... and %d more" % (len(problems) - 40))
    if problems:
        print("%d of the original's files have a record that does not name Martin Knafve: a table of "
              "REUSE.toml that names one of them has to name him too (build/generate-reuse-original.py)"
              % len(problems))
        return 1
    print("reuse names Martin Knafve as a holder of every one of the original's %d files but the %d "
          "third parties'" % (len(mine), len(theirs)))
    return 0


def self_test():
    failures = []

    def expect(what, got, want):
        if got != want:
            failures.append("%s: got %r, want %r" % (what, got, want))

    def record(copyrights):
        return {"files": [{"path": p, "copyrights": [{"value": v} for v in vs]} for p, vs in copyrights.items()]}

    originals = ["a/One.cpp", "a/TWO.CPP", "libraries/fat_cow_icons/ico/x.ico", "LICENSE", "gone.txt"]
    tree = ["a/One.cpp", "a/Two.cpp", "a/New.cpp", "libraries/fat_cow_icons/ico/x.ico", "LICENSE"]
    mine, theirs = original_files(originals, tree)
    expect("the original's files, letter case aside", mine, ["a/One.cpp", "a/Two.cpp"])
    expect("the third parties' files", theirs, ["libraries/fat_cow_icons/ico/x.ico"])

    # Split, so that reuse does not take them for this file's own.
    fork = "Copy" + "right (C) " + FORK
    knafve = "SPDX-File" + "CopyrightText: " + KNAFVE
    good = {"a/One.cpp": [fork, knafve], "a/Two.cpp": [knafve], "a/New.cpp": [fork],
            "libraries/fat_cow_icons/ico/x.ico": ["FatCow Web Hosting"]}
    expect("a record that names him", holders(originals, tree, record(good))[2], [])
    dropped = dict(good)
    dropped["a/Two.cpp"] = [fork]
    expect("a record of the original's file without him",
           [p.split(":")[0] for p in holders(originals, tree, record(dropped))[2]], ["a/Two.cpp"])
    unread = dict(good)
    del unread["a/One.cpp"]
    expect("an original's file reuse did not read",
           holders(originals, tree, record(unread))[2], ["a/One.cpp: reuse lint did not read it"])
    expect("a stale third party", "hmailserver/test/SSL examples/google.cer" in stale_third_parties(theirs), True)
    expect("a pattern's one level", bool(glob_regex("a/*.h").match("a/b/c.h")), False)
    expect("a pattern's any level", bool(glob_regex("a/**").match("a/b/c.h")), True)

    block = "\n".join([BEGIN, "old", END])
    toml = "version = 1\n\n" + block + "\n\n# after\n"
    expect("the tables spliced in", splice(toml, "\n".join([BEGIN, "new", END])),
           "version = 1\n\n" + "\n".join([BEGIN, "new", END]) + "\n\n# after\n")
    expect("REUSE.toml without the marker lines", splice("version = 1\n", block), None)
    try:
        toml_string("a/*.cpp")
        failures.append("a path with a * was written as a pattern")
    except SystemExit:
        pass

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if "--holders" in argv:
        i = argv.index("--holders")
        if i + 1 >= len(argv):
            print("--holders needs reuse lint --json's output")
            return 2
        return check_holders(argv[i + 1])
    return generate("--check" in argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
