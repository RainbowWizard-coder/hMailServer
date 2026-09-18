# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""No file is listed twice in a project, and every file listed is there.

A source listed twice is compiled twice at once into one object file, and the
build fails with "Cannot open compiler generated file ... Permission denied" -
which is what happened on both benches when a fold of two branches took the
union of their additions to the same list and one file had been added by both.
The build says nothing until then, and a duplicate can sit in the project for
as long as nobody builds from clean.

A path that names nothing is the other half: a file renamed or removed with the
project left behind builds until the linker, or silently leaves a translation
unit out of the binary. That half is asked only of what is compiled - the
sources and the headers - and not of the None entries, some of which have named
the registration scripts by the wrong path since before this fork, nor of the
two files MIDL generates into the build tree from the IDL.

Usage:  python build/check-project-items.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

PROJECTS = [
    ("hmailserver/source/Server/hMailServer/hMailServer.vcxproj", ("ClCompile", "ClInclude", "None", "ResourceCompile", "Midl")),
    ("hmailserver/test/RegressionTests/RegressionTests.csproj", ("Compile", "None", "EmbeddedResource")),
    ("hmailserver/test/LinuxRegressionTests/LinuxRegressionTests.csproj", ("Compile", "None", "EmbeddedResource")),
]

ITEM = re.compile(r"<(\w+)\s+Include=\"([^\"]+)\"")


def exists_ignoring_case(path):
    """Whether the path names a file, matching each component without regard
    to case: the projects are MSBuild's and are read on Windows, where
    ..\\Common\\Bo\\Account.cpp opens Common/BO/Account.cpp, and this check runs
    on Linux as well, where it would not. The case the project writes is the
    project's own business; what this asks is whether the file is there."""
    if os.path.exists(path):
        return True
    parts = []
    head = path
    while True:
        head, tail = os.path.split(head)
        if not tail:
            parts.insert(0, head)
            break
        parts.insert(0, tail)
    current = parts[0]
    for part in parts[1:]:
        if not os.path.isdir(current):
            return False
        wanted = part.lower()
        match = None
        for name in os.listdir(current):
            if name.lower() == wanted:
                match = name
                break
        if match is None:
            return False
        current = os.path.join(current, match)
    return os.path.exists(current)

# The kinds whose files are asked to exist, and the generated pair that do not
# until MIDL has run.
COMPILED = ("ClCompile", "ClInclude", "Compile")
GENERATED = ("hmailserver.h", "hmailserver_i.c")


def main():
    problems = []
    checked = 0
    for relative, kinds in PROJECTS:
        path = os.path.join(ROOT, relative.replace("/", os.sep))
        if not os.path.exists(path):
            problems.append("%s: not found" % relative)
            continue
        directory = os.path.dirname(path)
        with open(path, encoding="utf-8-sig", errors="replace") as f:
            text = f.read()
        seen = {}
        for kind, include in ITEM.findall(text):
            if kind not in kinds:
                continue
            checked += 1
            key = (kind, include.replace("/", "\\").lower())
            if key in seen:
                problems.append("%s: %s %s is listed twice" % (relative, kind, include))
                continue
            seen[key] = True
            # A wildcard is the project asking for whatever is there; a link
            # names a file elsewhere in the tree. Both are paths to check.
            if "*" in include or "$(" in include:
                continue
            if kind not in COMPILED or os.path.basename(include).lower() in GENERATED:
                continue
            target = os.path.normpath(os.path.join(directory, include.replace("\\", os.sep)))
            if not exists_ignoring_case(target):
                problems.append("%s: %s %s names nothing on disk" % (relative, kind, include))

    if problems:
        for p in problems:
            sys.stdout.write("  FAIL  %s\n" % p)
        sys.stdout.write("\n%d problem(s). A file listed twice breaks the build from clean; one that names nothing is a unit that is not built.\n" % len(problems))
        return 1

    sys.stdout.write("  OK    %d project item(s) across %d project(s): each listed once, each on disk\n" % (checked, len(PROJECTS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
