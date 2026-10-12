#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every C++ program of the server says which product and which release it is.

A Windows program's version resource (VS_VERSION_INFO) is what Explorer's Details
page, an inventory or software-asset tool, an anti-virus console and Windows Error
Reporting read to say what a file is: its product name, file version and product
version. hMailServer.exe has had one since upstream's first release, built from
Common/Application/Version.h, the one place RELEASE.md stamps a release. The two
programs shipped beside it in Bin had none until 12 October 2026 -
hMailServer.Minidump.exe, which writes the crash dumps, and hMailServer.Updater.exe,
which applies an update - so they said nothing of what they were or which release
they came from (upstream #658 gave its minidump writer one).

This holds every tracked .vcxproj that builds a program or a library to it: it
compiles a resource script (a ResourceCompile item) that defines VS_VERSION_INFO,
includes Common/Application/Version.h, and takes FILEVERSION and PRODUCTVERSION
from HMAILSERVER_VERSION_NUMERIC and the "FileVersion", "ProductVersion" and
"ProductName" strings from HMAILSERVER_VERSION and the product's name - so that a
release stamped in Version.h reaches every one of them, and a new program cannot
arrive without.

Run from the repository root: python build/check-version-resources.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# What a version resource must say, each with the reason a reader is given when it
# does not.
REQUIRED = [
    (r"\bVS_VERSION_INFO\s+VERSIONINFO\b", "defines no VS_VERSION_INFO VERSIONINFO"),
    (r'#\s*include\s+"[^"]*Common/Application/Version\.h"', "does not include Common/Application/Version.h"),
    (r"\bFILEVERSION\s+HMAILSERVER_VERSION_NUMERIC\b", "takes FILEVERSION from somewhere other than HMAILSERVER_VERSION_NUMERIC"),
    (r"\bPRODUCTVERSION\s+HMAILSERVER_VERSION_NUMERIC\b", "takes PRODUCTVERSION from somewhere other than HMAILSERVER_VERSION_NUMERIC"),
    (r'VALUE\s+"FileVersion"\s*,\s*HMAILSERVER_VERSION\b', 'takes "FileVersion" from somewhere other than HMAILSERVER_VERSION'),
    (r'VALUE\s+"ProductVersion"\s*,\s*HMAILSERVER_VERSION\b', 'takes "ProductVersion" from somewhere other than HMAILSERVER_VERSION'),
    (r'VALUE\s+"ProductName"\s*,\s*"hMailServer"', 'does not name the product "hMailServer"'),
]


def tracked_projects():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "*.vcxproj"],
                         stdout=subprocess.PIPE, check=True).stdout.decode("utf-8")
    return [line for line in out.splitlines() if line.strip()]


def read(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", errors="replace")


def problems_of(project):
    text = read(os.path.join(ROOT, project))

    kinds = set(re.findall(r"<ConfigurationType>\s*([A-Za-z]+)\s*</ConfigurationType>", text))
    if not kinds & {"Application", "DynamicLibrary"}:
        return None

    scripts = re.findall(r'<ResourceCompile\s+Include="([^"]+)"', text)
    if not scripts:
        return ["compiles no resource script (no ResourceCompile item), so the program has no version resource"]

    directory = os.path.dirname(os.path.join(ROOT, project))
    best = None
    for script in scripts:
        path = os.path.normpath(os.path.join(directory, script.replace("\\", os.sep)))
        if not os.path.isfile(path):
            return ["compiles %s, which is not there" % script]

        rc = read(path).replace("\\", "/")
        missing = [reason for pattern, reason in REQUIRED if not re.search(pattern, rc)]
        if not missing:
            return []
        if best is None or len(missing) < len(best[1]):
            best = (script, missing)

    return ["%s %s" % (best[0], reason) for reason in best[1]]


def main():
    failed = 0
    checked = 0

    for project in tracked_projects():
        problems = problems_of(project)
        if problems is None:
            continue

        checked += 1
        if problems:
            failed += 1
            print("FAIL  %s" % project)
            for problem in problems:
                print("        %s" % problem)
        else:
            print("OK    %s" % project)

    if checked == 0:
        print("FAIL  no C++ program found to check: the project list could not be read")
        return 1

    if failed:
        print("%d of %d C++ program(s) without a version resource from Version.h" % (failed, checked))
        return 1

    print("Every one of the %d C++ program(s) has a version resource from Version.h" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
