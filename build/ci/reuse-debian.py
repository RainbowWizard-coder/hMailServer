#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Debian source package's copyright file held to reuse's record of the tree.

Every file of the tree that the Debian source package carries - all that reuse
records but what debian/copyright's Files-Excluded leaves out - must have, in the
last Files paragraph of debian/copyright that matches it (the copyright format's
rule, https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/), the
licences that reuse lint --json records for it: the same set of licences, a
Debian short name standing for its SPDX identifier as DEBIAN_TO_SPDX says. So the
two records of what licence a file is under, REUSE.toml's and the package's,
cannot drift apart without the reuse job saying so. Martin Knafve's and the
maintainers' copyright are not compared: debian/copyright's "Files: *" names both
for every file, as a copyright file of one paragraph for the whole tree does.

Usage:
  python3 build/ci/reuse-debian.py LINT.json [COPYRIGHT]
                     exit 1 naming each file whose licences differ (COPYRIGHT is
                     the package's debian/copyright unless given)
  python3 build/ci/reuse-debian.py --self-test

build/ci/reuse-lint.sh runs it over reuse lint --json, and a negative control: a
copy of debian/copyright without the RFC 7208 suite's change log in its paragraph,
which must be refused by name.
"""

import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COPYRIGHT = "hmailserver/source/Server/platform/packaging/debian-src/debian/copyright"

# The Debian short names debian/copyright uses, and the SPDX identifier each stands
# for in REUSE.toml and LICENSES/.
DEBIAN_TO_SPDX = {
    "AGPL-3+": "AGPL-3.0-or-later",
    "Artistic": "Artistic-1.0-Perl",
    "BSD-1-clause": "BSD-1-Clause",
    "BSD-3-clause": "BSD-3-Clause",
    "Expat": "MIT",
    "GPL-1+": "GPL-1.0-or-later",
    "MPL-2.0": "MPL-2.0",
    "ScriptSite": "LicenseRef-ScriptSite",
    "StdString": "LicenseRef-StdString",
    "Unicode-3.0": "Unicode-3.0",
    "WHATWG-BSD-3-clause": "BSD-3-Clause",
    "Zlib": "Zlib",
    "notices": "LicenseRef-Verbatim-Notice",
}


def paragraphs(text):
    """debian/copyright's paragraphs, each a dict of field to value (continuation
    lines joined with newlines), in order."""
    out = []
    for block in re.split(r"\n[ \t]*\n", text.replace("\r\n", "\n")):
        fields = {}
        name = None
        for line in block.split("\n"):
            if not line.strip():
                continue
            if line[0] in " \t" and name:
                fields[name] += "\n" + line.strip()
                continue
            m = re.match(r"^([A-Za-z][A-Za-z0-9-]*):\s*(.*)$", line)
            if m:
                name = m.group(1)
                fields[name] = m.group(2).strip()
        if fields:
            out.append(fields)
    return out


def pattern_regex(pattern):
    """A Files or Files-Excluded pattern: "*" any run of characters, "/" too, and
    "?" any one; everything else literal."""
    return re.compile("^" + "".join(".*" if c == "*" else "." if c == "?" else re.escape(c) for c in pattern) + "$")


def parse(text):
    """(the Files-Excluded patterns, [(the Files patterns, the licence's short names)])."""
    excluded, files = [], []
    for fields in paragraphs(text):
        if "Files-Excluded" in fields:
            excluded = [pattern_regex(p) for p in fields["Files-Excluded"].split()]
        if "Files" in fields:
            short = fields.get("License", "").split("\n")[0]
            names = [n for n in re.split(r"\s+(?:and|or)\s+|\s*,\s*", short.strip()) if n]
            files.append(([pattern_regex(p) for p in fields["Files"].split()], fields["Files"].split()[0], names))
    return excluded, files


def is_excluded(path, excluded):
    """Files-Excluded leaves out a file it matches, and every file of a directory
    it matches."""
    parts = path.split("/")
    candidates = ["/".join(parts[:i]) for i in range(1, len(parts) + 1)]
    return any(regex.match(c) for regex in excluded for c in candidates)


def spdx_licences(entry):
    found = set()
    for expression in entry.get("spdx_expressions", []):
        for token in re.findall(r"[A-Za-z0-9.+-]+", expression.get("value", "")):
            if token not in ("AND", "OR", "WITH"):
                found.add(token)
    return found


def problems_of(lint, copyright_text):
    excluded, files = parse(copyright_text)
    problems = []
    carried = 0
    for entry in lint.get("files", []):
        path = entry["path"]
        if is_excluded(path, excluded):
            continue
        carried += 1
        match = None
        for regexes, first, names in files:
            if any(r.match(path) for r in regexes):
                match = (first, names)
        if match is None:
            problems.append("%s: the package carries it and no Files paragraph of debian/copyright matches it" % path)
            continue
        first, names = match
        unknown = [n for n in names if n not in DEBIAN_TO_SPDX]
        if unknown:
            problems.append("%s: debian/copyright's paragraph \"Files: %s\" names %s, which this check has no SPDX "
                            "identifier for (DEBIAN_TO_SPDX)" % (path, first, ", ".join(unknown)))
            continue
        debian = set(DEBIAN_TO_SPDX[n] for n in names)
        reuse = spdx_licences(entry)
        if debian != reuse:
            problems.append("%s: reuse records %s, and debian/copyright's paragraph \"Files: %s\" gives %s"
                            % (path, " + ".join(sorted(reuse)) or "no licence", first, " + ".join(sorted(debian))))
    return carried, problems


def check(lint_path, copyright_path):
    try:
        with io.open(lint_path, "r", encoding="utf-8") as f:
            lint = json.load(f)
    except (OSError, ValueError) as e:
        print("%s is not reuse lint --json's output: %s" % (lint_path, e))
        return 2
    try:
        with io.open(copyright_path, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        print("%s cannot be read: %s" % (copyright_path, e))
        return 2
    carried, problems = problems_of(lint, text)
    for problem in problems[:40]:
        print(problem)
    if len(problems) > 40:
        print("... and %d more" % (len(problems) - 40))
    if problems:
        print("%d of the %d files the Debian source package carries have other licences in debian/copyright than "
              "in REUSE.toml's record: give each the same in both" % (len(problems), carried))
        return 1
    if carried == 0:
        print("No file of reuse's record is one the Debian source package carries: the check saw nothing")
        return 1
    print("debian/copyright gives each of the %d files the Debian source package carries the licences reuse "
          "records for it" % carried)
    return 0


def self_test():
    failures = []

    def expect(what, got, want):
        if got != want:
            failures.append("%s: got %r, want %r" % (what, got, want))

    text = "\n".join([
        "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/",
        "Files-Excluded:",
        " libraries",
        " a/Some?file.txt",
        "",
        "Files: *",
        "Copyright: someone",
        "License: AGPL-3+",
        "",
        "Files: a/Mime.cpp",
        "       a/b/*.h",
        "Copyright: Jeff Lee",
        "License: BSD-1-clause and AGPL-3+",
        "Comment:",
        " a comment",
        "",
        "Files: a/b/odd.h",
        "Copyright: nobody",
        "License: Mystery",
        "",
        "Files: t/*",
        "Copyright: Perl's",
        "License: GPL-1+ or Artistic",
        "",
    ])

    def entry(path, *expressions):
        return {"path": path, "spdx_expressions": [{"value": e} for e in expressions]}

    lint = {"files": [
        entry("a/One.cpp", "AGPL-3.0-or-later"),
        entry("a/Mime.cpp", "BSD-1-Clause AND AGPL-3.0-or-later"),
        entry("a/b/c.h", "AGPL-3.0-or-later", "BSD-1-Clause"),
        entry("t/msg.txt", "GPL-1.0-or-later OR Artistic-1.0-Perl"),
        entry("libraries/x/y.c", "MIT"),
        entry("a/Some file.txt", "LicenseRef-Nothing"),
    ]}
    expect("records that agree", problems_of(lint, text), (4, []))
    wrong = {"files": lint["files"] + [entry("a/Two.cpp", "MIT")]}
    expect("a file whose licence differs", [p.split(":")[0] for p in problems_of(wrong, text)[1]], ["a/Two.cpp"])
    fewer = {"files": [entry("a/Mime.cpp", "AGPL-3.0-or-later")]}
    expect("a third party's licence missing from reuse's record", len(problems_of(fewer, text)[1]), 1)
    odd = {"files": [entry("a/b/odd.h", "AGPL-3.0-or-later")]}
    expect("a short name with no SPDX identifier", "no SPDX identifier" in problems_of(odd, text)[1][0], True)
    expect("the last paragraph that matches", problems_of({"files": [entry("a/b/c.h", "AGPL-3.0-or-later")]}, text)[1] != [],
           True)
    expect("a directory of Files-Excluded", is_excluded("libraries/x/y.c", parse(text)[0]), True)
    expect("a ? for a space", is_excluded("a/Some file.txt", parse(text)[0]), True)
    expect("not excluded by a prefix of a name", is_excluded("librariesX/y.c", parse(text)[0]), False)
    without = text.replace("       a/b/*.h\n", "")
    expect("a paragraph that lost a file", [p.split(":")[0] for p in problems_of(lint, without)[1]], ["a/b/c.h"])

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if not argv:
        print(__doc__.strip().split("\n\n")[-2])
        return 2
    copyright_path = argv[1] if len(argv) > 1 else os.path.join(ROOT, COPYRIGHT)
    return check(argv[0], copyright_path)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
