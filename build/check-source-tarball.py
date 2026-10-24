#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The source tarball's exclusion list against the tree, and a source tarball against the list.

build/ci/source-tarball.sh writes hmailserver-<version>.tar.gz: the tree of one
commit, less every path build/source-exclusions.txt names - SQL Server
Compact's headers and licence, which are not free, and what only the Windows
build and its installer use (hmailserver/docs/SourceTarball.md). Distributions
build from that tarball, and Debian's Files-Excluded, the RPM spec and the Guix
snippet are made from the same list. So the list has two ways to be wrong, and a
tarball a third:

  the list, against the tree (no argument; what the checkers job runs):
    - every line is a literal path in the list's own format (no wildcard, no
      "..", no leading "./" or trailing "/", nothing twice, nothing already
      under another line), and names a path git tracks;
    - nothing it names is read by the Linux build: no path that
      hmailserver/source/Server/CMakeLists.txt names through ${SERVER_DIR} or
      its own directory, no path build/ci/linux-build.sh or
      build/ci/linux-suite.sh names - nor the scripts of the other Linux jobs,
      the package builds, the fuzzing and source-tarball.sh itself
      (OTHER_LINUX_SCRIPTS below) - no file LinuxRegressionTests.csproj links,
      nothing in that project's own directory (the SDK compiles all of it),
      not global.json, nothing the build or the suite reads at run time
      (RUNTIME_READS below), and no header a source of the POSIX build
      includes outside a Windows-only
      branch (#ifndef HM_PLATFORM_POSIX, #ifdef _WIN32, #ifdef _MSC_VER and
      their like) - the one way the build reached into Server/zlib until
      28 September 2026;
    - every file under the trees that belong to Windows alone (WINDOWS_TREES
      below) is excluded or read by the Linux build, so a new installer script
      or Control Panel project is left out of the tarball the day it is added,
      or this says which line to add;
    - the licences the tarball must carry (REQUIRED below) are tracked and not
      excluded; QuickJS's among them, the vendored engine the Linux server runs
      its event scripts in;
    - where a line holds a space, the "?" Debian's Files-Excluded needs in its
      place matches that path and no other;
    - the Debian source package's Files-Excluded (DEBIAN_COPYRIGHT below) begins
      with the list as --files-excluded prints it, and each pattern after it -
      the Debian package's own - matches test data alone (files under
      hmailserver/test/ or fuzz/regression/), at least one, none of them read
      by the Debian build (CMakeLists.txt and debian/rules);
    - every pattern of that file's Files paragraphs (but "*" and the package's
      own debian/) matches a file the repacked source holds - one the tree
      tracks and neither the list nor Files-Excluded leaves out - so a file
      removed or renamed takes its licence paragraph with it (XMLite's outlived
      its files by a commit on 29 September 2026).

  a tarball (python3 build/check-source-tarball.py TARBALL [--commit SHA]):
    - gzip without a file name or a time in its header (gzip -n);
    - one top directory, hmailserver-<version>/, whose version is the
      HMAILSERVER_VERSION of the Version.h inside it and, when the file is named
      hmailserver-<version>.tar.gz, the one in its name;
    - only files and directories, all owned by uid 0 and gid 0, files 0644 or
      0755 and directories 0755, every one with the same time (the commit's);
    - no path the list excludes, and the list it carries is the list it is held
      to;
    - every REQUIRED file;
    - no program and no archive: nothing that begins as a Windows (PE), ELF or
      Mach-O executable, a Java class or a WebAssembly module does, nor as a
      zip (an empty or a spanned one too), 7-Zip, RAR, ar (a static library or
      a .deb) or rpm archive, a cabinet, an OLE compound file, a squashfs image
      or a gzip, compress (.Z), xz, bzip2, zstd, lz4 or lzip stream, nor a
      POSIX or GNU tar archive, which says "ustar" at offset 257 (what_binary;
      an old v7 tar says nothing a first block can be told by). The list of
      third-party licences is among them: until 23 October 2026 it was an
      OpenDocument spreadsheet, a zip by format, and the one archive this
      allowed by name;
    - with --commit, the commit git archive recorded in the tarball's pax header
      is the one given.

  python3 build/check-source-tarball.py --files-excluded
    prints the list as Debian's Files-Excluded field (copyright format 1.0),
    for debian/copyright.

  python3 build/check-source-tarball.py --self-test
    the negative controls: each rule broken in a tree or a tarball made in
    memory, and each must be reported; and a clean one that must pass.

Exit status: 0 when everything holds, 1 when anything does not, 2 on a usage
error or a file that cannot be read.
"""
import gzip
import io
import os
import posixpath
import re
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

EXCLUSIONS = "build/source-exclusions.txt"
SERVER = "hmailserver/source/Server"
CMAKELISTS = SERVER + "/CMakeLists.txt"
VERSION_H = SERVER + "/Common/Application/Version.h"
LINUX_SCRIPTS = ("build/ci/linux-build.sh", "build/ci/linux-suite.sh")
# And the scripts of the other jobs that build, package, fuzz or measure the Linux
# server - which a distribution building from the tarball runs as well - and the one
# linux-build.sh and linux-suite.sh unpack the tarball with, which runs the checker
# the tarball carries. Held the same way: a path any of them names stays.
OTHER_LINUX_SCRIPTS = (
    "build/ci/source-tarball.sh",
    "build/ci/linux-repro.sh",
    "build/ci/linux-cross.sh",
    "build/linux-tu-census.sh",
    "build/ci/linux-sanitize.sh",
    "build/ci/linux-coverage.sh",
    "build/ci/linux-fuzz.sh",
    "build/check-linux-version-stamp.sh",
    "build/ci/packages/deb-build.sh",
    "build/ci/packages/rpm-build.sh",
    "build/ci/packages/appimage-build.sh",
    "build/ci/packages/appimage-cross.sh",
    # The AppImage runtime's link and the record of what it takes in: every
    # AppImage names this tarball as where runtime.c and these scripts are, which
    # the LGPL-2.1 of libfuse3 (a relink) and the GPL of lzo (the corresponding
    # source) ask for (docs/PackagesAppImage.md, "Licences").
    "build/ci/packages/appimage-runtime.sh",
    "build/ci/packages/appimage-runtime-sources.sh",
    # And the pack of the source of the libraries the AppImages carry in usr/lib, which
    # appimage-build.sh runs to check their licences are known: every AppImage names
    # the release file it makes (docs/PackagesAppImage.md, "Licences").
    "build/ci/packages/appimage-bundled-sources.sh",
    "hmailserver/source/Server/platform/packaging/build-appimage.sh",
    "hmailserver/source/Server/platform/packaging/PKGBUILD",
    # The source packages (hmailserver/docs/SourcePackages.md): their builds, and the
    # jobs that build and install them.
    "hmailserver/source/Server/platform/packaging/debian-src/debian/rules",
    "hmailserver/source/Server/platform/packaging/rpm/hmailserver.spec",
    "build/ci/packages/srcpkg-deb.sh",
    "build/ci/packages/srcpkg-rpm.sh",
    "build/ci/packages/srcpkg-install.sh",
    # The Alpine package (hmailserver/docs/Alpine.md): the APKBUILD, which installs
    # the OpenRC script from the tarball, and the jobs that build it from the tarball,
    # install it and run the suite against it.
    "hmailserver/source/Server/platform/packaging/alpine/APKBUILD",
    "build/ci/alpine.sh",
)
# The Debian source package's copyright file, whose Files-Excluded is this list as
# --files-excluded prints it, then the Debian package's own patterns: test data the
# project's tarball keeps for its suites and a Debian archive cannot take, because
# no licence is stated for it. Those may only be files under DEBIAN_ONLY_UNDER - the
# test tree, and the fuzzers' regression inputs, one of which is a third party's reply
# as a fuzzer changed it - that
# the Debian build - the CMake build and debian/rules - does not read.
DEBIAN_COPYRIGHT = "hmailserver/source/Server/platform/packaging/debian-src/debian/copyright"
DEBIAN_RULES = "hmailserver/source/Server/platform/packaging/debian-src/debian/rules"
DEBIAN_ONLY_UNDER = ("hmailserver/test/", "fuzz/regression/")
SUITE_PROJECT = "hmailserver/test/LinuxRegressionTests/LinuxRegressionTests.csproj"
# The SDK compiles every .cs under the project's own directory and restores what its
# lock file pins, and dotnet reads the SDK's version from global.json at the top.
SUITE_READS_WHOLE = (posixpath.dirname(SUITE_PROJECT), "global.json")
# What the Linux build and suite read at run time from a path they build, which no
# pattern above can see, each read whole: the certificates the suite's TLS fixtures
# load (Shims/SslSetup.cs, AntiSpam/DKIM/Signing.cs), the settings baseline
# SettingsApplyOnSave.cs reads, the OpenPGP fixtures linux-build.sh's round trip
# opens, and the command-line client RestApiAccountImport.cs runs - which, missing,
# it skips rather than fails, so the suite would not say it had gone.
RUNTIME_READS = (
    "hmailserver/test/SSL examples",
    "build/ini-settings-baseline.txt",
    "build/fixtures/openpgp",
    "hmailserver/source/Tools/Cli",
)

# The sources the POSIX build compiles and the headers they reach: the directories
# CMakeLists.txt globs and the two it names file by file.
POSIX_SOURCE_DIRS = ("Common", "IMAP", "POP3", "SMTP", "ExternalFetcher", "platform", "quickjs")
SOURCE_SUFFIXES = (".c", ".cpp", ".h", ".hpp", ".inl")
# Where a quoted #include is looked for after the including file's own directory:
# CMakeLists.txt's target_include_directories.
INCLUDE_ROOTS = (SERVER + "/platform/pch", SERVER, SERVER + "/Common", SERVER + "/Common/Util")
# What the POSIX build defines, and what it never does. A branch on anything else
# is taken to be compiled.
POSIX_DEFINED = {"HM_PLATFORM_POSIX", "UNICODE", "_UNICODE", "BOOST_BIND_GLOBAL_PLACEHOLDERS"}
POSIX_UNDEFINED = {"_WIN32", "WIN32", "_WIN64", "_MSC_VER", "_M_ARM64", "_M_X64", "_M_IX86", "_ATL_VER"}

# Trees that belong to Windows alone: every file under each is excluded, or is read
# by the Linux build, or is kept by name here with the reason.
WINDOWS_TREES = {
    "libraries": ((), "the Windows build's dependencies and their licences"),
    SERVER + "/COM": ((), "the ATL administration API"),
    SERVER + "/hMailServer": ((), "the Windows service host"),
    SERVER + "/hMailServer.Minidump": ((), "Windows Error Reporting"),
    SERVER + "/hMailServer.Updater": ((), "the live update's Windows apply helper"),
    SERVER + "/zlib": ((), "zlib as hMailServer.exe compiles it"),
    "hmailserver/source/Tools": (("hmailserver/source/Tools/Cli",),
                                 "the Windows administration tools; Cli is the packages' hmctl"),
    "hmailserver/source/Addons": ((), "the Windows installer's VBScript add-ons"),
    "hmailserver/installation": ((), "the Windows installer; what CMakeLists.txt installs from it stays"),
}

# What every tarball carries: the project's licence, and the licence of every piece
# of third-party code the Linux build compiles or its tester programs run.
REQUIRED = (
    ("LICENSE", "the AGPL-3.0 the project is under"),
    (EXCLUSIONS, "the list the tarball was made without"),
    (CMAKELISTS, "the build"),
    (SERVER + "/quickjs/LICENSE", "QuickJS's MIT licence, beside the engine the Linux server runs event scripts in"),
    (SERVER + "/quickjs/VERSION", "the QuickJS release vendored, which the build and the SBOM read"),
    ("hmailserver/docs/Licenses/License - QuickJS.txt", "QuickJS's licence among the third-party notices"),
    ("hmailserver/docs/Licenses/License - MIME Message encoder analyzer.rtf", "Common/Mime's licence (Jeff Lee)"),
    ("hmailserver/docs/Licenses/License - String library.rtf", "Common/Util/StdString.h's licence (Joseph M. O'Leary)"),
    (SERVER + "/SMTP/SPF/Conformance/rfc7208-tests.LICENSE", "the RFC 7208 test suite's licence, which spf-tests runs"),
    ("hmailserver/docs/Licenses/License - RFC 7208 test suite.txt", "the same, among the third-party notices"),
    (SERVER + "/Common/Idna/ucd/unicode.LICENSE", "the Unicode License v3 of the UTS 46 data the server compiles in"),
    ("hmailserver/docs/Licenses/License - Unicode data.txt", "the same, among the third-party notices"),
    (SERVER + "/Common/AntiSpam/whatwg/entities.LICENSE",
     "the BSD 3-Clause terms of the HTML standard's list of named character references the server compiles in"),
    ("hmailserver/docs/Licenses/License - HTML named character references.txt", "the same, among the third-party notices"),
)


class Problem(Exception):
    pass


# ---------------------------------------------------------------- the list

def parse_list(text, where=EXCLUSIONS):
    """The list's paths, in order, and the problems with its lines."""
    entries, problems = [], []
    seen = set()
    for number, raw in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
        line = raw
        if line.strip() == "" or line.lstrip().startswith("#"):
            continue
        at = "%s:%d: %r" % (where, number, line)
        if line != line.strip():
            problems.append("%s has white space at an end: a path is the whole line, and nothing else" % at)
            continue
        if "\t" in line or "\\" in line:
            problems.append("%s holds a tab or a backslash, which no path of the tree has" % at)
            continue
        if re.search(r"[*?\[\]{}]", line):
            problems.append("%s holds a wildcard: the list is literal, so that shell, RPM and Guix read it alike" % at)
            continue
        parts = line.split("/")
        if line.startswith("/") or line.startswith("./") or line.endswith("/") or any(p in ("", ".", "..") for p in parts):
            problems.append("%s is not a plain path from the top of the tree (no leading / or ./, no trailing /, no . or ..)" % at)
            continue
        if line in seen:
            problems.append("%s is listed twice" % at)
            continue
        seen.add(line)
        entries.append(line)
    for entry in entries:
        for other in entries:
            if other != entry and entry.startswith(other + "/"):
                problems.append("%s: %s is under %s, which the list already names" % (where, entry, other))
    return entries, problems


def excluded_by(path, entries):
    """The line that excludes path, or None."""
    for entry in entries:
        if path == entry or path.startswith(entry + "/"):
            return entry
    return None


def debian_pattern(entry):
    """The entry as a Files-Excluded glob: white space separates globs there."""
    return entry.replace(" ", "?")


def debian_regex(pattern):
    """A Files-Excluded glob as Debian's copyright format 1.0 reads it: * and ?
    match any characters, "/" included, and a directory stands for what is below it."""
    out = ""
    for ch in pattern:
        out += ".*" if ch == "*" else "." if ch == "?" else re.escape(ch)
    return re.compile("^" + out + "(?:/.*)?$")


def files_excluded(entries):
    lines = ["Files-Excluded:"]
    for entry in entries:
        lines.append(" " + debian_pattern(entry))
    return "\n".join(lines)


def copyright_files_excluded(text):
    """The Files-Excluded patterns of a copyright file's header paragraph, in order."""
    header = text.replace("\r\n", "\n").split("\n\n", 1)[0]
    patterns, inside = [], False
    for line in header.split("\n"):
        if line.startswith("Files-Excluded:"):
            inside = True
            patterns += line[len("Files-Excluded:"):].split()
        elif inside and line[:1] in (" ", "\t"):
            patterns += line.split()
        else:
            inside = False
    return patterns


def copyright_files_patterns(text):
    """The patterns of every Files field of a copyright file, in order. White space
    separates them, and a field goes on over the lines that begin with it. The
    header's Files-Excluded is another field, not one of these."""
    patterns, inside = [], False
    for line in text.replace("\r\n", "\n").split("\n"):
        if line.startswith("Files:"):
            inside = True
            patterns += line[len("Files:"):].split()
        elif inside and line[:1] in (" ", "\t"):
            patterns += line.split()
        else:
            inside = False
    return patterns


def debian_files_regex(pattern):
    """A Files pattern as copyright format 1.0 reads it: * and ? match any
    characters and one, "/" included, and the pattern names the whole path."""
    out = ""
    for ch in pattern:
        out += ".*" if ch == "*" else "." if ch == "?" else re.escape(ch)
    return re.compile("^" + out + "$")


def check_debian_copyright_files(tracked, text, entries, excluded_patterns):
    """Every Files pattern names a file the repacked source holds."""
    excluded = [debian_regex(p) for p in excluded_patterns]
    present = [p for p in tracked if not excluded_by(p, entries) and not any(r.match(p) for r in excluded)]
    problems = []
    for pattern in copyright_files_patterns(text):
        # "*" is every file; debian/ is the packaging directory the source package
        # adds, which the tree keeps elsewhere.
        if pattern == "*" or pattern.startswith("debian/"):
            continue
        regex = debian_files_regex(pattern)
        if not any(regex.match(p) for p in present):
            problems.append("%s declares the licence of %r, which matches no file of the repacked source: a file "
                            "removed or renamed, whose Files line - and its paragraph, and its License paragraph when "
                            "nothing else names that licence - goes with it" % (DEBIAN_COPYRIGHT, pattern))
    return problems


def check_debian_copyright(tracked, read, entries, cmake_text):
    """debian-src/debian/copyright's Files-Excluded: the list, then test data alone;
    and its Files patterns, each a file the repacked source holds."""
    if DEBIAN_COPYRIGHT not in tracked:
        return []
    problems = []
    patterns = copyright_files_excluded(read(DEBIAN_COPYRIGHT))
    problems += check_debian_copyright_files(tracked, read(DEBIAN_COPYRIGHT), entries, patterns)
    wanted = [debian_pattern(entry) for entry in entries]
    if patterns[:len(wanted)] != wanted:
        problems.append("%s's Files-Excluded does not begin with %s as it stands: put in what "
                        "build/check-source-tarball.py --files-excluded prints" % (DEBIAN_COPYRIGHT, EXCLUSIONS))
        return problems
    debian_reads = cmake_reads(cmake_text)
    if DEBIAN_RULES in tracked:
        debian_reads |= script_reads(read(DEBIAN_RULES))
    for pattern in patterns[len(wanted):]:
        regex = debian_regex(pattern)
        matched = sorted(p for p in tracked if regex.match(p))
        if not matched:
            problems.append("%s's Files-Excluded has %r, which matches nothing in the tree: a file renamed or removed" %
                            (DEBIAN_COPYRIGHT, pattern))
        for path in matched:
            if not path.startswith(DEBIAN_ONLY_UNDER):
                problems.append("%s's Files-Excluded has %r, which leaves out %s: beyond %s it holds test data alone "
                                "(under %s), and a path the tarball should not carry goes in %s" %
                                (DEBIAN_COPYRIGHT, pattern, path, EXCLUSIONS, " or ".join(DEBIAN_ONLY_UNDER),
                                 EXCLUSIONS))
            elif any(path == r or path.startswith(r + "/") for r in debian_reads):
                problems.append("%s's Files-Excluded has %r, which leaves out %s, and the Debian build reads it" %
                                (DEBIAN_COPYRIGHT, pattern, path))
    return problems


# ---------------------------------------------------------------- what the Linux build reads

def cmake_reads(cmake_text):
    """Every path CMakeLists.txt names under its own directory, as a path from the top."""
    reads = set()
    # A ";" ends a path too: CMake joins a list with it ("a/postinst;${SERVER_DIR}/b/prerm").
    for match in re.finditer(r"\$\{(?:SERVER_DIR|CMAKE_CURRENT_LIST_DIR|CMAKE_CURRENT_SOURCE_DIR)\}/([^\s\"');]+)", cmake_text):
        relative = match.group(1)
        # A glob names its directory: Common/*.cpp reads Common.
        if "*" in relative:
            relative = relative[:relative.index("*")].rstrip("/")
        path = posixpath.normpath(posixpath.join(SERVER, relative))
        if path.startswith("../") or path == "..":
            continue
        reads.add(path)
    return reads


def cmake_list(cmake_text, name):
    match = re.search(r"set\(\s*" + re.escape(name) + r"\b(.*?)\)", cmake_text, re.S)
    if not match:
        return set()
    return set(SERVER + "/" + token for token in re.findall(r"^\s*([A-Za-z0-9_./-]+\.(?:cpp|c))\b", match.group(1), re.M))


def script_reads(text):
    """The paths from the top of the tree a Linux CI script names."""
    return set(match.group(1).rstrip(".")
               for match in re.finditer(r"(?<![\w/.$-])((?:hmailserver|build|fuzz)/[A-Za-z0-9_.+/-]*[A-Za-z0-9_+-])", text))


def csproj_reads(text, project=SUITE_PROJECT):
    reads = set()
    base = posixpath.dirname(project)
    for match in re.finditer(r'Include="([^"]+)"', text):
        include = match.group(1).replace("\\", "/")
        if not include.startswith("."):
            continue
        if "*" in include:
            include = include[:include.index("*")].rstrip("/")
        reads.add(posixpath.normpath(posixpath.join(base, include)))
    return reads


DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif|include)\b\s*(.*)$")


def branch_state(kind, argument):
    """True when the POSIX build compiles the branch, False when it never does,
    None when this cannot tell (and the branch counts as compiled)."""
    argument = re.sub(r"//.*$|/\*.*?\*/", "", argument).strip()
    if kind in ("ifdef", "ifndef"):
        symbol = argument.split()[0] if argument else ""
        known = True if symbol in POSIX_DEFINED else False if symbol in POSIX_UNDEFINED else None
        if known is None:
            return None
        return known if kind == "ifdef" else not known
    if argument in ("0", "1"):
        return argument == "1"
    match = re.match(r"^(!?)\s*defined\s*\(?\s*(\w+)\s*\)?$", argument)
    if not match:
        return None
    symbol = match.group(2)
    known = True if symbol in POSIX_DEFINED else False if symbol in POSIX_UNDEFINED else None
    if known is None:
        return None
    return (not known) if match.group(1) else known


def posix_includes(text):
    """Each quoted #include a POSIX compile can reach, with its line number."""
    stack = []  # frames: [live-now (True/False/None), a branch known live already]
    found = []
    for number, line in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
        match = DIRECTIVE.match(line)
        if not match:
            continue
        kind, argument = match.group(1), match.group(2)
        if kind in ("if", "ifdef", "ifndef"):
            state = branch_state(kind, argument)
            stack.append([state, state is True])
        elif kind == "elif" and stack:
            frame = stack[-1]
            frame[0] = False if frame[1] else None
        elif kind == "else" and stack:
            frame = stack[-1]
            frame[0] = False if frame[1] else (True if frame[0] is False else None)
        elif kind == "endif" and stack:
            stack.pop()
        elif kind == "include":
            if any(frame[0] is False for frame in stack):
                continue
            quoted = re.match(r'^"([^"]+)"', argument.strip())
            if quoted:
                found.append((number, quoted.group(1)))
    return found


def resolve_include(including, name, tracked):
    """The tracked file a quoted include reaches, or the path it names first when no
    tracked file is there (a generated header)."""
    candidates = [posixpath.normpath(posixpath.join(posixpath.dirname(including), name))]
    candidates += [posixpath.normpath(posixpath.join(root, name)) for root in INCLUDE_ROOTS]
    for candidate in candidates:
        if candidate in tracked:
            return candidate
    return candidates[0]


def check_tree(tracked, read, list_text):
    """The list against a tree: tracked is the set of paths git tracks, read(path)
    their text. Returns the problems and the entries."""
    entries, problems = parse_list(list_text)
    directories = set()
    for path in tracked:
        parts = path.split("/")
        for i in range(1, len(parts)):
            directories.add("/".join(parts[:i]))

    for entry in entries:
        if entry not in tracked and entry not in directories:
            problems.append("%s names %s, which the tree does not have: a path renamed or removed, and the list "
                            "no longer leaves out what it meant to" % (EXCLUSIONS, entry))
        if " " in entry:
            regex = debian_regex(debian_pattern(entry))
            strays = sorted(p for p in tracked if regex.match(p) and excluded_by(p, [entry]) is None)
            if strays:
                problems.append("%s: Debian's form of %r, %r, would also leave out %s" %
                                (EXCLUSIONS, entry, debian_pattern(entry), ", ".join(strays[:5])))

    # What the Linux build reads.
    # CMake's paths and the suite's links are read whole when they are directories (an
    # install(DIRECTORY), a glob's directory, a wildcard link), so nothing inside one
    # may go either. A script's paths are only the paths it names: its comments name
    # directories it does not copy.
    cmake_text = read(CMAKELISTS)
    reads, whole = {}, set()
    for path in cmake_reads(cmake_text):
        reads.setdefault(path, CMAKELISTS)
        whole.add(path)
    for path in csproj_reads(read(SUITE_PROJECT)) | set(SUITE_READS_WHOLE):
        reads.setdefault(path, SUITE_PROJECT)
        whole.add(path)
    for path in RUNTIME_READS:
        if path not in tracked and path not in directories:
            problems.append("%s is not in the tree, and RUNTIME_READS in build/check-source-tarball.py names it as read "
                            "at run time: name what took its place" % path)
        reads.setdefault(path, "what the Linux build and suite read at run time (RUNTIME_READS)")
        whole.add(path)
    for script in LINUX_SCRIPTS + OTHER_LINUX_SCRIPTS:
        if script not in tracked:
            problems.append("%s is not in the tree, and this check holds the list to what it reads: a script "
                            "renamed or removed - name its successor in build/check-source-tarball.py" % script)
            continue
        for path in script_reads(read(script)):
            reads.setdefault(path, script)
    for path, reader in sorted(reads.items()):
        entry = excluded_by(path, entries)
        if entry:
            problems.append("%s reads %s, and %s leaves it out with the line %r: the tarball would not build" %
                            (reader, path, EXCLUSIONS, entry))
            continue
        inside = [e for e in entries if e.startswith(path + "/")]
        if inside and path in whole and path in directories:
            problems.append("%s reads the directory %s whole, and %s leaves out %s inside it" %
                            (reader, path, EXCLUSIONS, ", ".join(inside)))

    left_out = cmake_list(cmake_text, "WINDOWS_ONLY_SOURCES")
    for path in sorted(tracked):
        if not path.startswith(SERVER + "/") or not path.endswith(SOURCE_SUFFIXES):
            continue
        if path.split("/")[3] not in POSIX_SOURCE_DIRS or path in left_out:
            continue
        for number, name in posix_includes(read(path)):
            target = resolve_include(path, name, tracked)
            entry = excluded_by(target, entries)
            if entry:
                problems.append("%s:%d includes \"%s\" (%s) where the POSIX build compiles it, and %s leaves out %r: "
                                "put it under #ifndef HM_PLATFORM_POSIX, or include the system's copy on POSIX" %
                                (path, number, name, target, EXCLUSIONS, entry))

    # The Windows-only trees, file by file.
    for tree, (kept, why) in sorted(WINDOWS_TREES.items()):
        for path in sorted(p for p in tracked if p.startswith(tree + "/")):
            if excluded_by(path, entries) or excluded_by(path, list(kept)):
                continue
            if any(path == r or path.startswith(r + "/") for r in reads):
                continue
            problems.append("%s is under %s (%s), and neither the list leaves it out nor the Linux build reads it: "
                            "add the line %r to %s" % (path, tree, why, path, EXCLUSIONS))

    for path, why in REQUIRED:
        if path not in tracked:
            problems.append("%s is not in the tree, and every tarball must carry it (%s)" % (path, why))
        elif excluded_by(path, entries):
            problems.append("%s leaves out %s, which every tarball must carry (%s)" % (EXCLUSIONS, path, why))
    problems += check_debian_copyright(tracked, read, entries, cmake_text)
    return problems, entries


def git_tracked(root):
    try:
        answer = subprocess.run(["git", "-C", root, "ls-files", "-z"], capture_output=True)
    except OSError as e:
        raise Problem("git cannot be run (%s): the list is checked against what git tracks" % e)
    if answer.returncode != 0:
        raise Problem("git ls-files failed in %s: %s" % (root, answer.stderr.decode("utf-8", "replace").strip()))
    return set(p for p in answer.stdout.decode("utf-8").split("\0") if p)


def read_file(root, path):
    try:
        with open(os.path.join(root, *path.split("/")), "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError as e:
        raise Problem("%s cannot be read: %s" % (path, e))


# ---------------------------------------------------------------- a tarball

EXECUTABLES = (
    (b"\x7fELF", "an ELF program or library"),
    (b"\xfe\xed\xfa\xce", "a Mach-O program"), (b"\xfe\xed\xfa\xcf", "a Mach-O program"),
    (b"\xce\xfa\xed\xfe", "a Mach-O program"), (b"\xcf\xfa\xed\xfe", "a Mach-O program"),
    (b"\xca\xfe\xba\xbe", "a Java class or a universal Mach-O program"),
    (b"\x00asm", "a WebAssembly module"),
)
# Each archive and compressed stream by the bytes it begins with: a zip (a local file
# header, or an empty or a spanned archive's first signature), 7-Zip, RAR, an ar
# archive (what a static library, .a or .lib, and a .deb are), an rpm package, a
# cabinet, an OLE compound file, a squashfs image (little-endian, the one order
# squashfs 4 writes), and gzip, compress (.Z, LZW and LZH), xz, bzip2, zstd, lz4 (its
# frame format) and lzip streams. A tar archive is told by TAR_MAGIC instead.
ARCHIVES = (
    (b"PK\x03\x04", "a zip archive"), (b"PK\x05\x06", "an empty zip archive"),
    (b"PK\x07\x08", "a spanned zip archive"), (b"7z\xbc\xaf\x27\x1c", "a 7-Zip archive"),
    (b"Rar!\x1a\x07", "a RAR archive"), (b"!<arch>\n", "an ar archive (a static library or a .deb)"),
    (b"\xed\xab\xee\xdb", "an rpm package"), (b"MSCF", "a cabinet"),
    (b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", "an OLE compound file (an .msi)"),
    (b"hsqs", "a squashfs image"),
    (b"\x1f\x8b", "a gzip stream"), (b"\x1f\x9d", "a compress (.Z) stream"), (b"\x1f\xa0", "a compress (.Z) stream"),
    (b"\xfd7zXZ\x00", "an xz stream"), (b"BZh", "a bzip2 stream"), (b"\x28\xb5\x2f\xfd", "a zstd stream"),
    (b"\x04\x22\x4d\x18", "an lz4 stream"), (b"LZIP", "an lzip stream"),
)
# A POSIX or GNU tar archive says so in its first header, "ustar" at offset 257 (the
# old v7 format says nothing, and only a tar reader would know one).
TAR_MAGIC = (257, b"ustar")


def what_binary(head):
    """What kind of program or archive the first bytes of a file make it, or None."""
    if head[:2] == b"MZ" and len(head) >= 0x40:
        offset = int.from_bytes(head[0x3c:0x40], "little")
        if offset + 4 <= len(head) and head[offset:offset + 4] == b"PE\x00\x00":
            return "a Windows (PE) program or library"
    for magic, name in EXECUTABLES + ARCHIVES:
        if head.startswith(magic):
            return name
    if head[TAR_MAGIC[0]:TAR_MAGIC[0] + len(TAR_MAGIC[1])] == TAR_MAGIC[1]:
        return "a tar archive"
    return None


def check_tarball(data, name, list_text, commit=None):
    """A tarball's bytes against the list. Returns the problems and what it found."""
    problems = []
    facts = {}
    if data[:2] != b"\x1f\x8b":
        return ["%s is not gzip" % name], facts
    flags = data[3]
    if flags & 0x08:
        problems.append("%s: the gzip header names a file (gzip was run without -n)" % name)
    if data[4:8] != b"\x00\x00\x00\x00":
        problems.append("%s: the gzip header holds a time (gzip was run without -n)" % name)
    entries, list_problems = parse_list(list_text)
    problems.extend(list_problems)
    try:
        archive = tarfile.open(fileobj=io.BytesIO(data), mode="r:gz")
        members = archive.getmembers()
    except (tarfile.TarError, OSError, EOFError) as e:
        return problems + ["%s cannot be read as a tar.gz: %s" % (name, e)], facts
    facts["commit"] = archive.pax_headers.get("comment", "")
    if not members:
        return problems + ["%s is empty" % name], facts

    tops = set(m.name.split("/")[0] for m in members)
    if len(tops) != 1:
        return problems + ["%s has %d top directories (%s), not one" % (name, len(tops), ", ".join(sorted(tops)[:5]))], facts
    top = tops.pop()
    facts["top"] = top
    match = re.match(r"^hmailserver-([0-9]+\.[0-9]+\.[0-9]+)$", top)
    if not match:
        problems.append("%s: the top directory is %r, not hmailserver-<version>" % (name, top))
    version = match.group(1) if match else None
    named = re.match(r"^hmailserver-([0-9]+\.[0-9]+\.[0-9]+)\.tar\.gz$", os.path.basename(name))
    if named and version and named.group(1) != version:
        problems.append("%s holds %s/, another version than its name says" % (name, top))

    times = set()
    files = {}
    for member in members:
        path = member.name[len(top) + 1:] if member.name.startswith(top + "/") else ""
        where = member.name
        if member.name != top and not member.name.startswith(top + "/"):
            problems.append("%s: %s is outside %s/" % (name, where, top))
            continue
        if not (member.isfile() or member.isdir()):
            problems.append("%s: %s is neither a file nor a directory (type %r)" % (name, where, member.type))
            continue
        if member.uid != 0 or member.gid != 0:
            problems.append("%s: %s is owned by %d:%d, not 0:0" % (name, where, member.uid, member.gid))
        if member.uname not in ("", "root") or member.gname not in ("", "root"):
            problems.append("%s: %s names its owner %s:%s, not root" % (name, where, member.uname, member.gname))
        mode = member.mode & 0o7777
        allowed = (0o755,) if member.isdir() else (0o644, 0o755)
        if mode not in allowed:
            problems.append("%s: %s has mode %04o, not %s" % (name, where, mode, " or ".join("%04o" % a for a in allowed)))
        times.add(member.mtime)
        if not path:
            continue
        entry = excluded_by(path, entries)
        if entry:
            problems.append("%s carries %s, which %s leaves out (%r)" % (name, path, EXCLUSIONS, entry))
        if member.isfile():
            files[path] = member
            head = archive.extractfile(member).read(4096)
            kind = what_binary(head)
            if kind:
                problems.append("%s carries %s, which is %s: a source tarball carries no program and no archive"
                                % (name, path, kind))
    if len(times) > 1:
        problems.append("%s: its entries carry %d different times, not the commit's one" % (name, len(times)))
    facts["mtime"] = times.pop() if len(times) == 1 else None
    facts["files"] = len(files)

    for path, why in REQUIRED:
        if path not in files:
            problems.append("%s does not carry %s (%s)" % (name, path, why))
    if EXCLUSIONS in files:
        carried = archive.extractfile(files[EXCLUSIONS]).read().decode("utf-8", "replace").replace("\r\n", "\n")
        if carried != list_text.replace("\r\n", "\n"):
            problems.append("%s carries another %s than the one it was checked against: check it with its own "
                            "(--exclusions)" % (name, EXCLUSIONS))
    if VERSION_H in files and version:
        text = archive.extractfile(files[VERSION_H]).read().decode("utf-8", "replace")
        stamped = re.search(r'^#define[ \t]+HMAILSERVER_VERSION[ \t]+"([^"]+)"', text, re.M)
        if not stamped or stamped.group(1) != version:
            problems.append("%s: %s/ holds a Version.h of %s" % (name, top, stamped.group(1) if stamped else "no version"))
    if commit is not None and facts["commit"] != commit:
        problems.append("%s was made from commit %s, not %s" % (name, facts["commit"] or "(none recorded)", commit))
    return problems, facts


# ---------------------------------------------------------------- the negative controls

def self_test():
    failures = []

    def expect(label, problems, needle):
        hit = [p for p in problems if needle in p] if needle else problems
        if needle and not hit:
            failures.append("%s: not reported (wanted %r); reported: %s" % (label, needle, problems[:3]))
        elif not needle and problems:
            failures.append("%s: a clean case was refused: %s" % (label, problems[:3]))
        else:
            print("  ok  %s" % label)

    cmake = ("file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/Common/Application/Version.h X)\n"
             "file(GLOB_RECURSE CORE_SOURCES ${SERVER_DIR}/Common/*.cpp)\n"
             "set(WINDOWS_ONLY_SOURCES\n   Common/SQL/Ce.cpp   # deferred\n)\n"
             "install(FILES ${SERVER_DIR}/quickjs/LICENSE DESTINATION x)\n"
             "install(FILES ${SERVER_DIR}/../../installation/WebAdmin/index.html DESTINATION y)\n"
             "install(DIRECTORY ${SERVER_DIR}/../DBScripts/ DESTINATION z)\n"
             'set(EXTRA "${SERVER_DIR}/platform/packaging/debian/postinst;${SERVER_DIR}/platform/packaging/debian/prerm")\n')
    tree = {
        "LICENSE": "AGPL", EXCLUSIONS: "", CMAKELISTS: cmake,
        VERSION_H: '#define HMAILSERVER_VERSION "1.2.3"\n',
        SERVER + "/quickjs/LICENSE": "MIT", SERVER + "/quickjs/VERSION": "2025-04-26",
        "hmailserver/docs/Licenses/License - QuickJS.txt": "MIT",
        "hmailserver/docs/Licenses/License - MIME Message encoder analyzer.rtf": "BSD",
        "hmailserver/docs/Licenses/License - String library.rtf": "free",
        "hmailserver/docs/Licenses/License - RFC 7208 test suite.txt": "BSD",
        "hmailserver/docs/Licenses/License - Microsoft SQL Server Compact Edition.rtf": "EULA",
        "hmailserver/docs/Licenses/License - Microsoft SQL Server Compact Edition 4.rtf": "EULA",
        SERVER + "/SMTP/SPF/Conformance/rfc7208-tests.LICENSE": "BSD",
        SERVER + "/Common/Idna/ucd/unicode.LICENSE": "Unicode-3.0",
        "hmailserver/docs/Licenses/License - Unicode data.txt": "Unicode-3.0",
        SERVER + "/Common/AntiSpam/whatwg/entities.LICENSE": "BSD-3-Clause",
        "hmailserver/docs/Licenses/License - HTML named character references.txt": "BSD-3-Clause",
        SERVER + "/Common/Util/Deflate.cpp": '#include "../../zlib/zlib.h"\n',
        SERVER + "/Common/Util/Win.cpp": '#ifndef HM_PLATFORM_POSIX\n#include "../../COM/Api.h"\n#endif\n',
        SERVER + "/Common/Util/Both.cpp": ('#ifdef HM_PLATFORM_POSIX\n#include <zlib.h>\n#else\n#include "../../zlib/zlib.h"\n#endif\n'
                                           '#ifdef _MSC_VER\n#include "../../COM/Api.h"\n#elif defined(__clang__)\n#else\n#endif\n'),
        SERVER + "/Common/SQL/Ce.cpp": '#include "../../zlib/zlib.h"\n',
        SERVER + "/zlib/zlib.h": "", SERVER + "/zlib/LICENSE": "zlib", SERVER + "/COM/Api.h": "",
        "libraries/mssqlce3.5/sqlce_oledb.h": "",
        "hmailserver/installation/WebAdmin/index.html": "", "hmailserver/installation/setup.iss": "",
        "hmailserver/source/DBScripts/CreateTablesPGSQL.sql": "", "hmailserver/source/DBScripts/Old.sql": "",
        "build/ci/linux-build.sh": "cd linux-build/x; bash build/check-linux-version-stamp.sh\n",
        "build/check-linux-version-stamp.sh": "",
        "build/ci/linux-suite.sh": "cp hmailserver/installation/WebAdmin/index.html x\n",
        SUITE_PROJECT: '<Compile Include="..\\RegressionTests\\Shared\\A.cs" />',
        "hmailserver/test/RegressionTests/Shared/A.cs": "",
        posixpath.dirname(SUITE_PROJECT) + "/Shims/TestSetup.cs": "", "global.json": "{}",
        SERVER + "/platform/packaging/debian/postinst": "", SERVER + "/platform/packaging/debian/prerm": "",
        "hmailserver/test/TestData/DKIM/good_1.txt": "",
        "build/fixtures/openpgp/alice.sec.asc": "",
        "hmailserver/test/SSL examples/localhost.pfx": "", "build/ini-settings-baseline.txt": "",
        "hmailserver/source/Tools/Cli/hmctl": "",
    }
    for script in OTHER_LINUX_SCRIPTS:
        tree.setdefault(script, "")
    tree["build/ci/linux-fuzz.sh"] = 'roots = ["hmailserver/test/TestData/DKIM"]\n'
    good = "\n".join([
        "# a comment", "", "libraries", SERVER + "/COM", SERVER + "/zlib", "hmailserver/installation/setup.iss",
        "hmailserver/docs/Licenses/License - Microsoft SQL Server Compact Edition.rtf",
        "hmailserver/docs/Licenses/License - Microsoft SQL Server Compact Edition 4.rtf", ""])
    fixed = dict(tree)
    fixed[SERVER + "/Common/Util/Deflate.cpp"] = '#ifdef HM_PLATFORM_POSIX\n#include <zlib.h>\n#else\n#include "../../zlib/zlib.h"\n#endif\n'

    def run(files, list_text):
        return check_tree(set(files), lambda p: files[p], list_text)[0]

    print("the list against a tree:")
    expect("a clean tree and list", run(fixed, good), None)
    expect("an unguarded include of a header the list leaves out",
           run(tree, good), "Deflate.cpp:1 includes \"../../zlib/zlib.h\"")
    expect("a wildcard", run(fixed, good + "hmailserver/installation/*.iss\n"), "holds a wildcard")
    expect("a path with a trailing slash", run(fixed, good + "fuzz/\n"), "not a plain path")
    expect("a path with trailing white space", run(fixed, good + "fuzz \n"), "white space at an end")
    expect("a line twice", run(fixed, good + "libraries\n"), "listed twice")
    expect("a line under another", run(fixed, good + "libraries/mssqlce3.5\n"), "is under libraries")
    expect("a path the tree does not have", run(fixed, good + "hmailserver/nothing-here\n"), "which the tree does not have")
    expect("a file CMakeLists.txt installs", run(fixed, good + "hmailserver/installation/WebAdmin\n"),
           "reads hmailserver/installation/WebAdmin/index.html")
    expect("a file a directory the build reads whole holds",
           run(fixed, good + "hmailserver/source/DBScripts/Old.sql\n"), "reads the directory hmailserver/source/DBScripts whole")
    expect("a file a Linux script reads", run(fixed, good + "build/check-linux-version-stamp.sh\n"),
           "linux-build.sh reads build/check-linux-version-stamp.sh")
    expect("a file the suite links", run(fixed, good + "hmailserver/test/RegressionTests/Shared\n"),
           "LinuxRegressionTests.csproj reads hmailserver/test/RegressionTests/Shared/A.cs")
    expect("a file of the suite project's own directory", run(fixed, good + posixpath.dirname(SUITE_PROJECT) + "/Shims\n"),
           "LinuxRegressionTests.csproj reads the directory hmailserver/test/LinuxRegressionTests whole")
    expect("global.json, which dotnet reads", run(fixed, good + "global.json\n"), "reads global.json")
    expect("a path another Linux job's script names", run(fixed, good + "hmailserver/test/TestData\n"),
           "linux-fuzz.sh reads hmailserver/test/TestData/DKIM")
    expect("one path of a CMake list joined with ;",
           run(fixed, good + SERVER + "/platform/packaging/debian/prerm\n"), "reads " + SERVER + "/platform/packaging/debian/prerm")
    expect("a file the round trip opens at run time", run(fixed, good + "build/fixtures/openpgp/alice.sec.asc\n"),
           "reads the directory build/fixtures/openpgp whole")
    expect("a file the suite reads at run time", run(fixed, good + "build/ini-settings-baseline.txt\n"),
           "at run time (RUNTIME_READS) reads build/ini-settings-baseline.txt")
    renamed = dict(fixed)
    del renamed["build/ini-settings-baseline.txt"]
    expect("a run-time read gone from the tree", run(renamed, good), "build/ini-settings-baseline.txt is not in the tree")
    gone = dict(fixed)
    gone.pop("build/ci/packages/rpm-build.sh", None)
    expect("a script the list is held to, gone from the tree", run(gone, good), "rpm-build.sh is not in the tree")
    expect("the QuickJS licence left out", run(fixed, good + SERVER + "/quickjs/LICENSE\n"), "quickjs/LICENSE, which every tarball must carry")
    missing = dict(fixed)
    del missing[SERVER + "/quickjs/LICENSE"]
    expect("the QuickJS licence missing from the tree", run(missing, good), "quickjs/LICENSE is not in the tree")
    expect("a file of a Windows-only tree kept by nobody",
           run(dict(fixed, **{"hmailserver/installation/new.iss": ""}), good), "hmailserver/installation/new.iss is under")
    ambiguous = dict(fixed, **{"hmailserver/docs/Licenses/License_-_Microsoft SQL Server Compact Edition.rtf": ""})
    expect("a Debian pattern that would match another file", run(ambiguous, good), "would also leave out")
    expect("the Files-Excluded field writes a space as ?",
           [] if " hmailserver/docs/Licenses/License?-?Microsoft?SQL?Server?Compact?Edition.rtf" in files_excluded(parse_list(good)[0])
           else ["no"], None)

    # The Debian source package's copyright file: the list, then test data alone.
    def with_copyright(field_lines, rules="", paragraphs=""):
        text = "Format: x\n" + "\n".join(field_lines) + "\nComment: y\n\nFiles: *\nLicense: z\n" + paragraphs
        return dict(fixed, **{DEBIAN_COPYRIGHT: text, DEBIAN_RULES: rules,
                              "hmailserver/test/TestData/DKIM/Good/Test 1.eml": "",
                              "fuzz/regression/mime_message_fuzzer/a-reply": "",
                              "fuzz/harness/fuzzer.cpp": ""})
    field = files_excluded(parse_list(good)[0]).split("\n")
    expect("a copyright file whose Files-Excluded is the list and test data",
           run(with_copyright(field + [" hmailserver/test/TestData/DKIM/Good/Test?1.eml"]), good), None)
    expect("a copyright file missing a line of the list",
           run(with_copyright([f for f in field if "COM" not in f]), good), "does not begin with build/source-exclusions.txt")
    expect("a copyright file that leaves out a fuzzer's regression input too",
           run(with_copyright(field + [" hmailserver/test/TestData/DKIM/Good/Test?1.eml",
                                       " fuzz/regression/mime_message_fuzzer/a-reply"]), good), None)
    expect("a copyright file leaving out what is not test data",
           run(with_copyright(field + [" global.json"]), good), "test data alone")
    expect("a copyright file leaving out a fuzzer's source",
           run(with_copyright(field + [" fuzz/harness/fuzzer.cpp"]), good), "test data alone")
    expect("a copyright file leaving out a file that is not there",
           run(with_copyright(field + [" hmailserver/test/none.txt"]), good), "matches nothing in the tree")
    expect("a copyright file leaving out test data debian/rules reads",
           run(with_copyright(field + [" hmailserver/test/TestData/DKIM/good_1.txt"],
                              rules="\tcp hmailserver/test/TestData/DKIM/good_1.txt x\n"), good),
           "the Debian build reads it")

    # Its Files paragraphs: each pattern a file the repacked source holds.
    declared = ("\nFiles: " + SERVER + "/quickjs/*\n       hmailserver/test/TestData/DKIM/good_1.txt\n"
                "       hmailserver/test/TestData/DKIM/Good/Test?1.e?l\nLicense: q\n\nFiles: debian/*\nLicense: z\n")
    expect("a copyright file whose Files patterns each match a file", run(with_copyright(field, paragraphs=declared), good), None)
    expect("a copyright file declaring a file that is gone (XMLite's, on 29 September 2026)",
           run(with_copyright(field, paragraphs=declared + "\nFiles: " + SERVER + "/Common/Util/XMLite.cpp\n"
                              "       " + SERVER + "/Common/Util/XMLite.h\nLicense: XMLite\n"), good),
           "'" + SERVER + "/Common/Util/XMLite.cpp', which matches no file")
    expect("a copyright file declaring a file on a continuation line that is gone",
           run(with_copyright(field, paragraphs="\nFiles: " + SERVER + "/quickjs/LICENSE\n\t" + SERVER + "/Common/Gone.h\nLicense: q\n"), good),
           "'" + SERVER + "/Common/Gone.h', which matches no file")
    expect("a copyright file declaring only a file the list leaves out",
           run(with_copyright(field, paragraphs="\nFiles: " + SERVER + "/zlib/*\nLicense: Zlib\n"), good),
           "'" + SERVER + "/zlib/*', which matches no file")
    expect("a copyright file declaring only a file its own Files-Excluded leaves out",
           run(with_copyright(field + [" hmailserver/test/TestData/DKIM/Good/Test?1.eml"],
                              paragraphs="\nFiles: hmailserver/test/TestData/DKIM/Good/*\nLicense: none\n"), good),
           "'hmailserver/test/TestData/DKIM/Good/*', which matches no file")
    expect("a Files pattern names the whole path, not a directory",
           run(with_copyright(field, paragraphs="\nFiles: " + SERVER + "/quickjs\nLicense: q\n"), good),
           "'" + SERVER + "/quickjs', which matches no file")

    print("a tarball against the list:")

    def tarball(files=None, extra=None, uid=0, mode=0o644, mtime=1790000000, top="hmailserver-1.2.3", gzip_name=False,
                gzip_time=0, commit="0123456789abcdef0123456789abcdef01234567", symlink=False, second_top=False,
                late=None):
        files = dict(files if files is not None else {
            "LICENSE": b"AGPL", EXCLUSIONS: good.encode(), CMAKELISTS: b"cmake",
            VERSION_H: b'#define HMAILSERVER_VERSION "1.2.3"\n',
            SERVER + "/quickjs/LICENSE": b"MIT", SERVER + "/quickjs/VERSION": b"2025-04-26",
            "hmailserver/docs/Licenses/License - QuickJS.txt": b"MIT",
            "hmailserver/docs/Licenses/License - MIME Message encoder analyzer.rtf": b"BSD",
            "hmailserver/docs/Licenses/License - String library.rtf": b"free",
            "hmailserver/docs/Licenses/License - RFC 7208 test suite.txt": b"BSD",
            SERVER + "/SMTP/SPF/Conformance/rfc7208-tests.LICENSE": b"BSD",
            SERVER + "/Common/Idna/ucd/unicode.LICENSE": b"Unicode-3.0",
            "hmailserver/docs/Licenses/License - Unicode data.txt": b"Unicode-3.0",
            SERVER + "/Common/AntiSpam/whatwg/entities.LICENSE": b"BSD-3-Clause",
            "hmailserver/docs/Licenses/License - HTML named character references.txt": b"BSD-3-Clause",
            "hmailserver/docs/Licenses/License list.md": b"License list\n============\n",
        })
        files.update(extra or {})
        raw = io.BytesIO()
        with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT,
                          pax_headers={"comment": commit} if commit else None) as out:
            directories = set()
            for path in files:
                parts = path.split("/")
                for i in range(1, len(parts)):
                    directories.add("/".join(parts[:i]))
            for path in [""] + sorted(directories):
                info = tarfile.TarInfo((top + "/" + path).rstrip("/"))
                info.type, info.mode, info.mtime, info.uid, info.gid, info.uname, info.gname = tarfile.DIRTYPE, 0o755, mtime, uid, uid, "root", "root"
                out.addfile(info)
            for path, content in sorted(files.items()):
                info = tarfile.TarInfo(top + "/" + path)
                info.size, info.mode, info.mtime, info.uid, info.gid, info.uname, info.gname = len(content), mode, mtime, uid, uid, "root", "root"
                if path == late:
                    info.mtime = mtime + 1
                out.addfile(info, io.BytesIO(content))
            if symlink:
                info = tarfile.TarInfo(top + "/link")
                info.type, info.linkname, info.mtime = tarfile.SYMTYPE, "LICENSE", mtime
                out.addfile(info)
            if second_top:
                info = tarfile.TarInfo("other/README")
                info.size, info.mtime = 1, mtime
                out.addfile(info, io.BytesIO(b"x"))
        packed = io.BytesIO()
        with gzip.GzipFile(filename="x.tar" if gzip_name else "", mode="wb", fileobj=packed, mtime=gzip_time) as z:
            z.write(raw.getvalue())
        return packed.getvalue()

    def check(data, name="hmailserver-1.2.3.tar.gz", commit=None):
        return check_tarball(data, name, good, commit)[0]

    elf = b"\x7fELF" + b"\x00" * 60
    pe = bytearray(b"MZ" + b"\x00" * 126)
    pe[0x3c:0x40] = (0x40).to_bytes(4, "little")
    pe[0x40:0x44] = b"PE\x00\x00"
    expect("a clean tarball", check(tarball()), None)
    expect("a planted excluded path", check(tarball(extra={"libraries/mssqlce3.5/sqlce_oledb.h": b"x"})),
           "carries libraries/mssqlce3.5/sqlce_oledb.h, which build/source-exclusions.txt leaves out")
    expect("the planted licence of SQL Server Compact",
           check(tarball(extra={"hmailserver/docs/Licenses/License - Microsoft SQL Server Compact Edition.rtf": b"x"})),
           "Compact Edition.rtf, which build/source-exclusions.txt leaves out")
    no_quickjs = tarball(files={k: v for k, v in [("LICENSE", b"AGPL"), (EXCLUSIONS, good.encode()), (CMAKELISTS, b"c")]})
    expect("QuickJS's licence missing", check(no_quickjs), "does not carry hmailserver/source/Server/quickjs/LICENSE")
    expect("an owner other than root", check(tarball(uid=1000)), "is owned by 1000:1000")
    expect("a group-writable file", check(tarball(mode=0o664)), "has mode 0664")
    expect("an entry with a time of its own", check(tarball(late="LICENSE")), "2 different times")
    expect("a symbolic link", check(tarball(symlink=True)), "neither a file nor a directory")
    expect("two top directories", check(tarball(second_top=True)), "2 top directories")
    expect("a top directory without the version", check(tarball(top="hmailserver")), "not hmailserver-<version>")
    expect("a name of another version", check(tarball(), "hmailserver-9.9.9.tar.gz"), "another version than its name")
    expect("a Version.h of another version",
           check(tarball(extra={VERSION_H: b'#define HMAILSERVER_VERSION "1.2.4"\n'})), "holds a Version.h of 1.2.4")
    expect("gzip with the file's name", check(tarball(gzip_name=True)), "names a file")
    expect("gzip with a time", check(tarball(gzip_time=1790000000)), "holds a time")
    expect("an ELF program", check(tarball(extra={"fuzz/a.out": elf})), "an ELF program")
    expect("a Windows program", check(tarball(extra={"build/tool.exe": bytes(pe)})), "a Windows (PE) program")
    expect("a zip archive", check(tarball(extra={"build/icons.zip": b"PK\x03\x04rest"})), "a zip archive")
    expect("the licence list as the OpenDocument spreadsheet it was, a zip by format, at its old path",
           check(tarball(extra={"hmailserver/docs/Licenses/License list.ods": b"PK\x03\x04 spreadsheet"})),
           "carries hmailserver/docs/Licenses/License list.ods, which is a zip archive")
    expect("an OLE compound file (an .msi)",
           check(tarball(extra={"build/setup.msi": b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1rest"})), "an OLE compound file")
    # Every other archive and stream what_binary knows, each by the bytes it begins
    # with, under a name that does not say what it is.
    ustar = bytearray(b"sources/README" + b"\x00" * 600)
    ustar[257:265] = b"ustar\x0000"
    gnu_tar = bytearray(b"sources/README" + b"\x00" * 600)
    gnu_tar[257:265] = b"ustar  \x00"
    for label, head, kind in (
            ("an empty zip archive", b"PK\x05\x06" + b"\x00" * 18, "an empty zip archive"),
            ("a spanned zip archive", b"PK\x07\x08PK\x03\x04rest", "a spanned zip archive"),
            ("a 7-Zip archive", b"7z\xbc\xaf\x27\x1crest", "a 7-Zip archive"),
            ("a RAR archive", b"Rar!\x1a\x07\x01\x00rest", "a RAR archive"),
            ("a POSIX tar archive (ustar at offset 257)", bytes(ustar), "a tar archive"),
            ("a GNU tar archive", bytes(gnu_tar), "a tar archive"),
            ("a static library, an ar archive", b"!<arch>\n/               0           0     0     0       4         `\n",
             "an ar archive (a static library or a .deb)"),
            ("a .deb, an ar archive", b"!<arch>\ndebian-binary   1790000000  0     0     100644  4         `\n2.0\n",
             "an ar archive (a static library or a .deb)"),
            ("an rpm package", b"\xed\xab\xee\xdb\x03\x00\x00\x00rest", "an rpm package"),
            ("a cabinet", b"MSCF\x00\x00\x00\x00rest", "a cabinet"),
            ("a squashfs image", b"hsqs" + b"\x00" * 92, "a squashfs image"),
            ("a gzip stream", b"\x1f\x8b\x08\x00rest", "a gzip stream"),
            ("a compress (.Z) stream, LZW", b"\x1f\x9d\x90rest", "a compress (.Z) stream"),
            ("a compress (.Z) stream, LZH", b"\x1f\xa0rest", "a compress (.Z) stream"),
            ("an xz stream", b"\xfd7zXZ\x00\x00\x04rest", "an xz stream"),
            ("a bzip2 stream", b"BZh91AY&SYrest", "a bzip2 stream"),
            ("a zstd stream", b"\x28\xb5\x2f\xfd\x24\x00rest", "a zstd stream"),
            ("an lz4 stream", b"\x04\x22\x4d\x18\x64\x40rest", "an lz4 stream"),
            ("an lzip stream", b"LZIP\x01\x0crest", "an lzip stream")):
        expect(label, check(tarball(extra={"build/sources.dat": head})),
               "carries build/sources.dat, which is %s: a source tarball carries no program and no archive" % kind)
    told_apart = b"A text that says ustar and !<arch> and hsqs, none of them where an archive has it.\n" * 8
    expect("a text naming those signatures, none at its start or at offset 257",
           check(tarball(extra={"build/notes.txt": told_apart})), None)
    expect("another list carried", check(tarball(extra={EXCLUSIONS: b"libraries\n"})), "carries another")
    expect("the commit recorded", check(tarball(), commit="0123456789abcdef0123456789abcdef01234567"), None)
    expect("another commit recorded", check(tarball(), commit="f" * 40), "was made from commit 0123456789")
    expect("not gzip at all", check(b"plain text"), "is not gzip")

    if failures:
        print("")
        for failure in failures:
            print("SELF-TEST FAILED: " + failure)
        return 1
    print("\nEvery negative control was reported, and the clean cases passed.")
    return 0


# ---------------------------------------------------------------- main

USAGE = """usage:
  python3 build/check-source-tarball.py                        the list against the tree
  python3 build/check-source-tarball.py TARBALL [--commit SHA] [--exclusions FILE]
  python3 build/check-source-tarball.py --files-excluded       the list as Debian's field
  python3 build/check-source-tarball.py --self-test"""


def main(argv):
    exclusions = os.path.join(ROOT, *EXCLUSIONS.split("/"))
    commit = None
    tarballs = []
    mode = "tree"
    args = list(argv)
    while args:
        arg = args.pop(0)
        if arg == "--self-test":
            mode = "self-test"
        elif arg == "--files-excluded":
            mode = "files-excluded"
        elif arg in ("--commit", "--exclusions") and args:
            value = args.pop(0)
            if arg == "--commit":
                commit = value
            else:
                exclusions = value
        elif arg in ("-h", "--help"):
            print(USAGE)
            return 0
        elif arg.startswith("-"):
            print(USAGE, file=sys.stderr)
            return 2
        else:
            tarballs.append(arg)
    if mode == "self-test":
        return self_test()
    try:
        with open(exclusions, "rb") as f:
            list_text = f.read().decode("utf-8")
    except (OSError, UnicodeDecodeError) as e:
        print("%s cannot be read: %s" % (exclusions, e))
        return 2
    if mode == "files-excluded":
        entries, problems = parse_list(list_text)
        if problems:
            print("\n".join(problems), file=sys.stderr)
            return 1
        print(files_excluded(entries))
        return 0

    if tarballs:
        failed = False
        for path in tarballs:
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except OSError as e:
                print("%s cannot be read: %s" % (path, e))
                return 2
            problems, facts = check_tarball(data, path, list_text, commit)
            if problems:
                failed = True
                print("%s: %d problem(s)" % (path, len(problems)))
                for problem in problems:
                    print("  " + problem)
            else:
                print("%s: %s/, %d files, commit %s, time %s; nothing the list leaves out, every licence it must carry, "
                      "no program and no archive, owners root and modes fixed." %
                      (path, facts.get("top"), facts.get("files"), facts.get("commit") or "(none recorded)", facts.get("mtime")))
        return 1 if failed else 0

    try:
        tracked = git_tracked(ROOT)
        problems, entries = check_tree(tracked, lambda p: read_file(ROOT, p), list_text)
    except Problem as e:
        print(str(e))
        return 2
    if problems:
        print("%s: %d problem(s)" % (EXCLUSIONS, len(problems)))
        for problem in problems:
            print("  " + problem)
        return 1
    left = sum(1 for p in tracked if excluded_by(p, entries))
    print("%s: %d lines leave out %d of the %d tracked files; none is read by the Linux build, every file of the "
          "Windows-only trees is accounted for, and every licence the tarball must carry stays." %
          (EXCLUSIONS, len(entries), left, len(tracked)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
