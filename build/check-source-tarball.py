#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The source tarball's exclusion list against the tree, and a source tarball against the list;
and the testers the source packages built from it run.

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
      its files by a commit on 29 September 2026);
    - the source packages' testers: debian/rules' TESTERS and the RPM spec's
      %check loop run the same tester programs in the same order, each once,
      each one CMakeLists.txt makes by add_executable(<name>-tests ...), and
      every one it so makes but those SOURCE_TESTERS_LEFT_OUT below leaves
      out, each with its reason (a database, a loopback socket, root, or a
      pass held to a time on a builder a distribution shares), and nothing
      that list names is run or no longer made - so a tester added to the
      build is run by the PPA's, Copr's and the Build Service's builds as
      linux-build.sh runs it, or this says why it is not. CMakeLists.txt is
      read as CMake reads it: each command, its name in any case, with its
      quoted, bracket and unquoted arguments, comments of both kinds left out,
      so that a key given quoted is read unquoted. Only the commands the file
      uses are read (CMAKE_COMMANDS); any other - try_compile, exec_program,
      add_custom_command, execute_process, a module's command - is refused, and
      add_subdirectory, subdirs, cmake_language, a _-prefixed command, a
      function and a macro with a word of why. Of those it reads, it refuses
      what would make or write a *-tests program otherwise than add_executable
      under its own name: an add_executable whose name is not a literal (a
      variable, a foreach's); a program named or placed otherwise than its
      target by set_target_properties or set_property (OUTPUT_NAME but the
      target's own, RUNTIME_OUTPUT_NAME, SUFFIX, PREFIX, a <CONFIG>_POSTFIX,
      RUNTIME_OUTPUT_DIRECTORY and their like), or by a key or a list CMake
      works out from variables or gets in a bracket; the variables that name
      or place every program (CMAKE_EXECUTABLE_SUFFIX,
      CMAKE_RUNTIME_OUTPUT_DIRECTORY, EXECUTABLE_OUTPUT_PATH, a module's or a
      library's prefix and suffix, CMAKE_<CONFIG>_POSTFIX) named anywhere; CMake
      code from elsewhere (an include() of anything but CMAKE_INCLUDES, as a
      bare name no module has is a file beside CMakeLists.txt, CMAKE_MODULE_PATH,
      CMAKE_PROJECT_*INCLUDE); a find_package() of a package not in
      CMAKE_PACKAGES or given PATHS, HINTS, NAMES or CONFIGS, and a package's
      <name>_DIR or <name>_ROOT, CMAKE_PREFIX_PATH or an ENV{} named anywhere,
      which could take a package's configuration from the tree; a file() that
      does other than read, list, make a directory or write a file it names out
      that is not a *-tests (it makes it 0644, which linux-build.sh's find
      passes over); a configure_file whose input or output is not named out, or
      is a *-tests; an add_custom_target built with every build (ALL); and an
      install(CODE) or install(SCRIPT), which rpm runs before %check. Of the
      toolchain it lets through what CMakeLists.txt gives it: set_target_properties
      sets OUTPUT_NAME alone (CMAKE_TARGET_PROPERTIES), set_property appends to
      the directory's CMAKE_CONFIGURE_DEPENDS alone, add_compile_options,
      add_link_options and target_compile_options give CMAKE_FLAGS alone,
      target_link_libraries gives libraries, targets and no flag (one there
      comes after the linker's own -o: -ohidden-tests made hidden-tests), and
      of variables only ${LDAP_LIBRARY}, ${LBER_LIBRARY} and ${CMAKE_DL_LIBS}
      (CMAKE_LINK_VARIABLES), the first two written by their own find_library
      as the file has it and by nothing else, CMake's own by nothing; the CMake
      variables written (by set, unset, option, list, string, file, foreach,
      find_library, find_path or check_c_source_runs) are the five the file
      sets (CMAKE_WRITTEN), so that a tool CMake runs with every compile
      (CMAKE_CXX_CLANG_TIDY) or another compile rule (CMAKE_CXX_COMPILE_OBJECT)
      is refused, and so is a variable written by a name CMake works out
      (set(${X} ...), a variable alone in string()); a launcher, another linker
      or compiler, CMAKE_<LANG>_FLAGS, -o with or without anything after it,
      -Wl,, -B, -fuse-ld and a specs file are refused anywhere, and
      check_c_source_runs, which compiles and runs its program while CMake
      configures, runs the iconv check's alone, held by its SHA-256
      (CMAKE_RUN_SOURCES), with CMAKE_REQUIRED_LIBRARIES Iconv::Iconv and no
      other CMAKE_REQUIRED_. A carriage return that ends no line is refused, as
      CMake takes it for a space between two arguments. Like the readers of
      the other files, this one accepts only the forms CMakeLists.txt is written
      with and refuses the rest, so a change to CMakeLists.txt that needs a new
      command, flag, variable or form changes this check with it. It does not
      read what CMake's own modules and the distribution's compiler and linker
      do, nor the -D options the spec's %build gives CMake. And the loops run what the lists name, as held: debian/rules is
      held whole, line for line but for its comments, its blank lines and the
      names TESTERS gives (RULES_HELD, whose override_dh_auto_test is
      RULES_TEST_RECIPE: set -e, the server, then ./$$tester for each of
      $(TESTERS), each a command of its own, skipped only for nocheck and a
      cross build), so that a TESTERS made another for the recipe (indented,
      target-specific, a second one, an eval), .IGNORE, MAKEFLAGS, a
      conditional around the rule, set +e or an exit is refused, and so is
      another rule's recipe that runs dh with DEB_BUILD_OPTIONS=nocheck or
      MAKEFLAGS=TESTERS=, or rewrites debian/rules before dh runs the held one;
      a comment ending in a backslash, which joins the next line to it, and a
      carriage return are refused, and a comment that a backslash joins to the
      line above is read as a line. The spec's one %check is SPEC_CHECK_LINES
      but for its comments and the loop's list, outside any conditional, as
      rpm reads one after expanding the line's macros (%if, %ifarch and their
      like with spaces or tabs and any run of % before them, as %%if is %if to
      rpm; none after something else on its line, as %{?nil}%%if and
      %{?quickjs_version:%%if 0} are; and every %endif closing one counted, the
      count ending at nought), its
      comments expanding no macro (rpm expands a comment's macros, and one that
      ends in a line break and an exit, as %{___build_post} does, or a shell's
      output, %(...), ends the section there), with no macro defined but
      SPEC_MACROS (the build directory %check goes to as the spec defines it),
      spelt %define, %global, %{define:...} or %{global:...} with or without
      blanks after the colon, on any line, a comment's too, none taken away,
      read from another file or made by macro or shell code (%undefine,
      %include, %lua, %expand, %load, %bcond, %(...)), no BuildSystem or
      BuildOption tag and no carriage return - so a loop over another list, a
      tester not run, a continue, set +e or an exit is refused. What the build
      does to the build directory, and what the files debian/ holds besides
      debian/rules tell debhelper, are not read.

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
import hashlib
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
# test tree, and the fuzzers' regression inputs, two of which are third parties' replies
# as a fuzzer changed them - that
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
    ("hmailserver/docs/Licenses/License - Feather icons.txt",
     "the MIT licence of Feather, whose icons the webmail's page, compiled into the server, draws after"),
)


# The source packages' testers. debian/rules' TESTERS, which dh_auto_test runs, and
# the spec's %check loop name the tester programs the PPA's, Copr's and the Build
# Service's builds run, one by one, where linux-build.sh, the CI's package builds, the
# APKBUILD and the Nix package run every *-tests program CMake makes: so a tester
# CMakeLists.txt makes is in both lists, in the same order, or left out here with the
# reason. Written by hand, the lists had missed acme-tests, a tester of the release
# after 6.3.7, and quarantine-search-tests, report-parser-tests and
# workqueue-teardown-tests, which 6.3.6 and 6.3.7 made; the last is left out below,
# as the testers held to a time are.
SPEC = "hmailserver/source/Server/platform/packaging/rpm/hmailserver.spec"
SOURCE_TESTERS_LEFT_OUT = {
    "database-claim-tests": "it needs a database: given no configuration it says SKIPPED and tests nothing",
    "database-transaction-tests": "it needs a database: given no configuration it says SKIPPED and tests nothing",
    "http-server-tests": "it serves and asks on a loopback socket, which the Debian build's offline.c refuses",
    "network-deadline-tests": "it holds connections on loopback sockets to their deadlines, which offline.c refuses",
    "clamd-socket-tests": "it binds Unix sockets of its own under the temporary directory, and the lists hold the "
                          "testers that need no socket",
    "root-command-tests": "most of it needs root, which a distribution's build does not run as",
    "imap-parse-tests": "every case in it is a pass held to a time over an input made to be 6.3.5's worst, the "
                        "bound one of them broke under emulation (a FETCH of 200,000 \"[]\" in 2.7 s against 2 s, "
                        "4 October 2026), on a builder a distribution shares with other builds",
    "input-cost-tests": "every case in it is a pass held to a time over an input made to be 6.3.5's worst, on a "
                        "builder a distribution shares with other builds",
    "workqueue-teardown-tests": "it holds two tasks of twenty seconds to outlast removes whose stops each give up "
                                "after ten, a margin of ten seconds of wall clock that a builder a distribution "
                                "shares with other builds, or emulates, can take up, where a failure says only that "
                                "the builder was slow; linux-build.sh, linux-sanitize and the CI's own package builds "
                                "run it on the project's runners, and ClassTester on Windows",
}


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


# ---------------------------------------------------------------- the source packages' testers

# CMakeLists.txt read as CMake reads it (cmake_commands): each command's name, in any
# case, then its arguments to the bracket that closes it - each a quoted argument, a
# bracket argument ([[ ... ]], [=[ ... ]=]) or an unquoted one, comments of both kinds
# left out - so that a key or a name given quoted, or a command after a bracket
# argument, is read as CMake reads it. A target's name is a literal or it is refused:
# one CMake works out - a variable, a foreach's - cannot be held to the lists.
CMAKE_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
CMAKE_BRACKET = re.compile(r"\[(=*)\[")
CMAKE_LITERAL = re.compile(r"^[A-Za-z0-9_.+-]+$")
# The commands this reads, the ones CMakeLists.txt uses: any other is refused, as one
# that could make or write a program this does not see (try_compile's COPY_FILE,
# exec_program, add_custom_command, find_program, a command of a module). Those that
# could make or write one among these have their arguments read below.
CMAKE_COMMANDS = frozenset((
    "add_compile_options", "add_custom_target", "add_executable", "add_library", "add_link_options",
    "check_c_source_runs", "cmake_minimum_required", "configure_file", "else", "elseif", "endforeach", "endif",
    "file", "find_library", "find_package", "find_path", "foreach", "if", "include", "install", "list", "message",
    "option", "project", "set", "set_property", "set_target_properties", "string", "target_compile_definitions",
    "target_compile_options", "target_include_directories", "target_link_libraries", "unset"))
# The CMake modules include() may load, CMake's own that CMakeLists.txt uses: include()
# of anything else could load a file of the tree (a bare name CMake finds no module of is
# taken for a file beside CMakeLists.txt), a tester among it.
CMAKE_INCLUDES = ("CheckCSourceRuns", "GNUInstallDirs", "CPack")
# The packages find_package() may look for, by CMake's own modules and the system's
# configuration files alone: a package's file from the tree (PATHS, HINTS, a <name>_DIR or
# <name>_ROOT, CMAKE_PREFIX_PATH) could make a tester.
CMAKE_PACKAGES = ("Threads", "OpenSSL", "ZLIB", "Boost", "PostgreSQL", "Iconv")
CMAKE_PACKAGE_PATHS = ("PATHS", "HINTS", "NAMES", "CONFIGS")
# What file() may do: read, list, make a directory, and write or append to a file it names
# out, which it makes 0644, so that linux-build.sh's find (-perm -u+x) never runs one;
# never copy, install, download, link or change a file's mode.
CMAKE_FILE_READS = ("STRINGS", "READ", "GLOB", "GLOB_RECURSE", "MAKE_DIRECTORY")
CMAKE_FILE_WRITES = ("WRITE", "APPEND")
# The properties that name a target's file or the directory it is made in: a tester
# named or placed otherwise than its target is one the lists and linux-build.sh's find
# would not agree on. A <CONFIG>_POSTFIX names an executable too (RELEASE_POSTFIX -tests
# under the Release build the Linux builds configure).
CMAKE_NAMING = re.compile(r"^(?:(?:RUNTIME_|LIBRARY_|ARCHIVE_)?OUTPUT_NAME(?:_\w+)?|SUFFIX|PREFIX|IMPORT_(?:PREFIX|SUFFIX)|"
                          r"(?:RUNTIME|LIBRARY|ARCHIVE)_OUTPUT_DIRECTORY(?:_\w+)?|\w*POSTFIX)$", re.I)
# The variables that do the same for every target, bring in CMake code from elsewhere, or
# point find_package() at a directory, named anywhere but in a comment.
CMAKE_NAMING_VARIABLES = re.compile(r"\b(?:CMAKE_EXECUTABLE_SUFFIX\w*|CMAKE_RUNTIME_OUTPUT_DIRECTORY\w*|"
                                    r"EXECUTABLE_OUTPUT_PATH|CMAKE_MODULE_PATH|CMAKE_PROJECT_\w*INCLUDE\w*|"
                                    r"CMAKE_USER_MAKE_RULES_OVERRIDE\w*|CMAKE_\w*(?:LIBRARY|MODULE)_(?:PREFIX|SUFFIX)\w*|"
                                    r"CMAKE_\w+_POSTFIX|CMAKE_PREFIX_PATH|CMAKE_SYSTEM_PREFIX_PATH|"
                                    r"CMAKE_FIND_(?:ROOT_PATH\w*|PACKAGE_\w+|USE_\w+)|"
                                    r"(?:Threads|OpenSSL|OPENSSL|ZLIB|Boost|BOOST|PostgreSQL|POSTGRESQL|Iconv|ICONV)_"
                                    r"(?:DIR|ROOT\w*)|BOOSTROOT)\b|\bENV\{|"
                                    # And the toolchain told to run another program or write another output:
                                    # a launcher, another linker or compiler, flags for every compile or link,
                                    # the try_run of check_c_source_runs given other inputs, and the options
                                    # that name an output or a program (-o, -Wl,, -B, -fuse-ld, a specs file).
                                    r"\b(?:CMAKE_\w*LAUNCHER\w*|RULE_LAUNCH_\w+|CMAKE_\w*LINKER\w*|CMAKE_\w*_FLAGS\w*|"
                                    r"CMAKE_\w+_COMPILER|CMAKE_REQUIRED_(?!LIBRARIES\b)\w+)\b|"
                                    # -o with anything after it: -ohidden-tests is -o hidden-tests to the compiler.
                                    r"(?<![\w-])-o|(?<![\w-])(?:--output|-Xlinker|-wrapper)\b|-Wl,|(?<![\w-])-B|-fuse-ld|"
                                    r"--ld-path|-specs\b|\b(?:LINKER|SHELL):")
# The target properties set_target_properties may set, and how: OUTPUT_NAME, to the
# target's own name, which is all CMakeLists.txt sets; any other (LINK_OPTIONS, a
# launcher, a naming property) is refused. set_property only appends to the directory's
# CMAKE_CONFIGURE_DEPENDS, as CMakeLists.txt does.
CMAKE_TARGET_PROPERTIES = ("OUTPUT_NAME",)
CMAKE_SET_PROPERTY = ("DIRECTORY", "APPEND", "PROPERTY", "CMAKE_CONFIGURE_DEPENDS")
# The flags add_compile_options, add_link_options and target_compile_options may give,
# the ones CMakeLists.txt gives (the operand of -include and -mllvm after it): another
# could name another output or program for the compiler or the linker.
CMAKE_FLAGS = frozenset(("-fno-common", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                         "-fno-omit-frame-pointer", "-fprofile-instr-generate", "-fcoverage-mapping", "-mllvm",
                         "-runtime-counter-relocation", "-fwrapv", "-include",
                         "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."))
CMAKE_FLAG_OPERANDS = ("-include", "-mllvm")
CMAKE_FLAG_KEYWORDS = ("PUBLIC", "PRIVATE", "INTERFACE", "BEFORE")
# check_c_source_runs compiles and runs its C program while CMake configures, in a
# directory under the build directory, so it could write a *-tests program there: the one
# program CMakeLists.txt runs so, the iconv check's, is held by its SHA-256 (as read, from
# the line break after its [=[ to its ]=]), with CMAKE_REQUIRED_LIBRARIES Iconv::Iconv.
CMAKE_RUN_SOURCES = {"HM_ICONV_CONVERTS_UTF7": "d0a9eb5e46dab6ca9bf20769c405e3a516f6a76b44b1cade7413f8f5cf73cfd2"}
CMAKE_REQUIRED = ("CMAKE_REQUIRED_LIBRARIES", "Iconv::Iconv")
# The commands that write a variable by its name, and the CMake variables CMakeLists.txt
# writes, the five it sets: any other CMAKE_* written (CMAKE_CXX_CLANG_TIDY, a tool CMake
# runs in the build directory with every compile; CMAKE_CXX_COMPILE_OBJECT, the compile
# rule itself) could run a program that writes a *-tests one, so it is refused, as is a
# variable written by a name CMake works out (set(${X} ...)).
CMAKE_WRITES = ("set", "unset", "option", "list", "string", "file", "foreach", "find_library", "find_path",
                "check_c_source_runs")
CMAKE_WRITTEN = ("CMAKE_CXX_STANDARD", "CMAKE_CXX_STANDARD_REQUIRED", "CMAKE_CXX_EXTENSIONS",
                 "CMAKE_POSITION_INDEPENDENT_CODE", "CMAKE_REQUIRED_LIBRARIES")
# The variables target_link_libraries may be given, the ones CMakeLists.txt gives it, and
# what alone may write each: the find_library that finds it, as the file has it, or for
# CMake's own CMAKE_DL_LIBS nothing. A variable there could hold a flag (-ohidden-tests).
CMAKE_LINK_VARIABLES = {"LDAP_LIBRARY": ("LDAP_LIBRARY", "NAMES", "ldap"),
                        "LBER_LIBRARY": ("LBER_LIBRARY", "NAMES", "lber"), "CMAKE_DL_LIBS": None}
# The commands that make targets or run CMake code this does not read: from another
# directory, file or project, by another name (cmake_language(CALL ...), the _-prefixed
# originals of a command defined over), or in a function or macro, whose arguments it
# cannot read. (Refused as any command not in CMAKE_COMMANDS is, with a word of why.)
CMAKE_ELSEWHERE = re.compile(r"^(?:add_subdirectory|subdirs|cmake_language|function|macro|load_command|"
                             r"variable_watch|fetchcontent_\w+|externalproject_\w+|_\w+)$", re.I)


class CMakeUnread(Exception):
    pass


def cmake_commands(cmake_text):
    """(name, [(argument, kind), ...], line) of each command of a CMakeLists.txt, read as CMake reads it: kind
    is "quoted", "bracket" or "unquoted", a quoted or bracket argument given without its quotes or brackets, and
    ( and ) unquoted arguments of their own; comments are left out. CMakeUnread for a text CMake would not read
    as commands."""
    text, found, at = cmake_text.replace("\r\n", "\n"), [], 0

    def line(offset):
        return text.count("\n", 0, offset) + 1

    def bracket(offset):
        """(content, end) of a bracket argument or comment opening at offset, or None."""
        opened = CMAKE_BRACKET.match(text, offset)
        if not opened:
            return None
        close = text.find("]%s]" % opened.group(1), opened.end())
        if close < 0:
            raise CMakeUnread("line %d: a bracket opened there is never closed" % line(offset))
        return text[opened.end():close], close + len(opened.group(1)) + 2

    def comment(offset):
        """The end of the comment opening at offset."""
        block = bracket(offset + 1)
        if block:
            return block[1]
        end = text.find("\n", offset)
        return len(text) if end < 0 else end

    while at < len(text):
        char = text[at]
        if char in " \t\n":
            at += 1
            continue
        if char == "#":
            at = comment(at)
            continue
        name = CMAKE_NAME.match(text, at)
        if not name:
            raise CMakeUnread("line %d: %r is not the start of a command" % (line(at), text[at:at + 20]))
        start, at = at, name.end()
        while at < len(text) and text[at] in " \t":
            at += 1
        if at >= len(text) or text[at] != "(":
            raise CMakeUnread("line %d: %s is not followed by its (" % (line(start), name.group(0)))
        at += 1
        arguments, depth = [], 1
        while True:
            if at >= len(text):
                raise CMakeUnread("line %d: %s( is never closed" % (line(start), name.group(0)))
            char = text[at]
            if char in " \t\n":
                at += 1
            elif char == "#":
                at = comment(at)
            elif char == "(":
                depth += 1
                arguments.append(("(", "unquoted"))
                at += 1
            elif char == ")":
                depth -= 1
                at += 1
                if not depth:
                    break
                arguments.append((")", "unquoted"))
            elif char == '"':
                end = at + 1
                while end < len(text) and text[end] != '"':
                    end += 2 if text[end] == "\\" else 1
                arguments.append((text[at + 1:end], "quoted"))
                at = end + 1
            elif bracket(at):
                content, at = bracket(at)
                arguments.append((content, "bracket"))
            else:
                end, quoted = at, False
                while end < len(text) and (quoted or text[end] not in " \t\n()#"):
                    if text[end] == "\\":
                        end += 1
                    elif text[end] == '"':
                        quoted = not quoted
                    end += 1
                arguments.append((text[at:end], "unquoted"))
                at = end
        found.append((name.group(0), arguments, line(start)))
    return found


def cmake_targets(cmake_text):
    """(the *-tests programs CMakeLists.txt makes in its order, problems): a command this does not read, a target
    named by other than a literal, a tester named or placed otherwise than its target, CMake code from elsewhere,
    a package looked for in a directory, and a file written that could be a *-tests program are problems."""
    cmake_text = cmake_text.replace("\r\n", "\n")
    testers, problems = [], []
    if "\r" in cmake_text:
        # CMake takes a carriage return alone for a space between arguments (OUTPUT_NAME<CR>hidden-tests is a
        # key and its value), where a reader that took it for part of an argument would read one.
        return [], ["%s line %d has a carriage return that is not followed by a line feed: CMake reads it as a "
                    "space between two arguments, which build/check-source-tarball.py does not - end each line with "
                    "a line feed" % (CMAKELISTS, cmake_text.count("\n", 0, cmake_text.find("\r")) + 1)]
    try:
        commands = cmake_commands(cmake_text)
    except CMakeUnread as error:
        return [], ["%s %s: CMake would read it otherwise than build/check-source-tarball.py does" % (CMAKELISTS, error)]

    def worked_out(arguments):
        """Whether an argument is one CMake works out: unquoted, a variable or a list of them that becomes any
        number of arguments; or anywhere, a key or a name made of variables."""
        return [value for value, kind in arguments if kind == "unquoted" and "$" in value]

    for command, arguments, line in commands:
        lower = command.lower()
        values = [value for value, _ in arguments]
        where = "%s line %d: %s(%s)" % (CMAKELISTS, line, command, " ".join(values)[:50])
        if CMAKE_ELSEWHERE.match(command):
            problems.append("%s line %d: %s() makes targets or runs CMake code this does not read (another "
                            "directory or project, another command's name, a function's or macro's arguments), a "
                            "tester among them perhaps - write each tester's add_executable out in %s"
                            % (CMAKELISTS, line, command, CMAKELISTS))
            continue
        if lower not in CMAKE_COMMANDS:
            problems.append("%s is a command build/check-source-tarball.py does not read (CMAKE_COMMANDS): it could "
                            "make or write a *-tests program, which linux-build.sh's find would run and the source "
                            "packages' lists would not - add it to CMAKE_COMMANDS with what it may do, or make the "
                            "testers with add_executable" % where)
            continue
        if lower == "add_executable":
            name = values[0] if values else ""
            if arguments[:1] and arguments[0][1] == "bracket" or not CMAKE_LITERAL.match(name):
                problems.append("%s line %d: add_executable(%s ...) is not named by a literal (a variable, or a "
                                "foreach's): whether it makes a *-tests program, which linux-build.sh would run, "
                                "cannot be read - write each tester's add_executable out" % (CMAKELISTS, line, name))
            elif name.endswith("-tests"):
                testers.append(name)
        elif lower == "set_target_properties":
            upper = [value.upper() for value in values]
            at = upper.index("PROPERTIES") if "PROPERTIES" in upper else len(values)
            targets = values[:at]
            if worked_out(arguments) or any(kind == "bracket" for _, kind in arguments):
                problems.append("%s gives a target's properties by an argument CMake works out (%s): a variable "
                                "that names OUTPUT_NAME, or a list that moves every key after it, cannot be read - "
                                "write each property out" % (where, ", ".join(worked_out(arguments)) or "a bracket"))
            for key, value in zip(values[at + 1::2], values[at + 2::2]):
                if CMAKE_NAMING.match(key) and not (key.upper() == "OUTPUT_NAME" and targets == [value]):
                    problems.append("%s line %d: set_target_properties(%s ...) gives %s %s %s: the lists run a "
                                    "tester by its target's name, and linux-build.sh the file it finds in the build "
                                    "directory - make each program under its own name, there"
                                    % (CMAKELISTS, line, " ".join(targets), "/".join(targets), key, value))
                elif "$" in key:
                    problems.append("%s names a property by %s, which CMake works out: it could be OUTPUT_NAME"
                                    % (where, key))
                elif key not in CMAKE_TARGET_PROPERTIES:
                    problems.append("%s sets %s, a target property build/check-source-tarball.py does not read "
                                    "(CMAKE_TARGET_PROPERTIES: OUTPUT_NAME, to the target's name): it could give the "
                                    "linker another output or run another program - add it there with what it may "
                                    "be" % (where, key))
        elif lower == "set_property":
            scope = values[0].upper() if values else ""
            if arguments and (arguments[0][1] == "bracket" or "$" in values[0]):
                problems.append("%s gives its scope by an argument CMake works out" % where)
            elif scope == "TARGET":
                upper = [value.upper() for value in values]
                key = values[upper.index("PROPERTY") + 1] if "PROPERTY" in upper[:-1] else ""
                if worked_out(arguments) or any(kind == "bracket" for _, kind in arguments) or "$" in key:
                    problems.append("%s gives a target's property by an argument CMake works out (%s): it could "
                                    "be OUTPUT_NAME - write it out" % (where, ", ".join(worked_out(arguments)) or key))
                elif CMAKE_NAMING.match(key):
                    problems.append("%s line %d: set_property(TARGET ... PROPERTY %s) names or places a program "
                                    "otherwise than its target: the lists run a tester by its target's name, and "
                                    "linux-build.sh the file it finds in the build directory" % (CMAKELISTS, line, key))
                else:
                    problems.append("%s sets a target's property, which build/check-source-tarball.py reads only "
                                    "as set_target_properties gives it (CMAKE_TARGET_PROPERTIES)" % where)
            elif tuple(value.upper() for value in values[:4]) != CMAKE_SET_PROPERTY or any(
                    kind == "bracket" for _, kind in arguments):
                problems.append("%s sets a property build/check-source-tarball.py does not read (it reads set_property"
                                "(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ...) alone): a directory's "
                                "COMPILE_OPTIONS or LINK_OPTIONS could give the compiler or the linker another output"
                                % where)
        elif lower in ("add_compile_options", "add_link_options", "target_compile_options"):
            given = values[1:] if lower == "target_compile_options" else values
            for index, value in enumerate(given):
                operand = index and given[index - 1] in CMAKE_FLAG_OPERANDS and not value.startswith("-")
                if value not in CMAKE_FLAGS and value not in CMAKE_FLAG_KEYWORDS and not operand:
                    problems.append("%s gives %s, a flag build/check-source-tarball.py does not read (CMAKE_FLAGS): "
                                    "the compiler or the linker could write another output or run another program "
                                    "(-o, -Wl,, -B, -fuse-ld) - add it there" % (where, value[:40]))
                    break
        elif lower == "target_link_libraries":
            # A variable only as ${NAME} of CMAKE_LINK_VARIABLES, which only its find_library writes: another
            # could hold a flag (set(X -ohidden-tests) then ${X}), and a generator expression could make one.
            odd = [value for value, kind in arguments if kind != "unquoted" or value.startswith("-")
                   or re.match(r"^(?:LINKER|SHELL):", value)
                   or "$" in value and not (re.match(r"^\$\{(\w+)\}$", value)
                                            and re.match(r"^\$\{(\w+)\}$", value).group(1) in CMAKE_LINK_VARIABLES)]
            if odd:
                problems.append("%s gives the linker %s, which is not a library, a target or one of the variables "
                                "build/check-source-tarball.py lets it have (CMAKE_LINK_VARIABLES: %s): a flag there "
                                "comes after the linker's -o, and could name another output"
                                % (where, odd[0][:40], ", ".join(sorted(CMAKE_LINK_VARIABLES))))
        elif lower == "check_c_source_runs":
            source = arguments[0][0] if arguments and arguments[0][1] == "bracket" else None
            result = values[1] if len(values) == 2 else None
            if (result not in CMAKE_RUN_SOURCES or source is None
                    or hashlib.sha256(source.encode("utf-8")).hexdigest() != CMAKE_RUN_SOURCES[result]):
                problems.append("%s line %d: check_c_source_runs(... %s) compiles and runs a program while CMake "
                                "configures, in the build directory, which could write a *-tests program there: "
                                "build/check-source-tarball.py holds the one it runs by its SHA-256 "
                                "(CMAKE_RUN_SOURCES) - change both together" % (CMAKELISTS, line, result or ""))
        if lower in CMAKE_WRITES:
            # The variable a command writes by a name CMake works out cannot be read; one CMake reads as its own
            # (CMAKE_*) is one of the five the file sets, and one target_link_libraries is given is written by its
            # own find_library alone.
            # Where each writes it: set, unset, option, foreach, find_library and find_path the first argument,
            # list the second, file(READ|STRINGS) the third and file(GLOB|GLOB_RECURSE) the second, the result
            # of check_c_source_runs the second; string() at a place that differs by its mode, so in string() no
            # unquoted argument may be a variable alone.
            upper = values[0].upper() if values else ""
            if lower == "list":
                named = values[1:2]
            elif lower == "file":
                named = values[2:3] if upper in ("READ", "STRINGS") else values[1:2] if upper.startswith("GLOB") else []
            elif lower == "check_c_source_runs":
                named = values[1:2]
            elif lower == "string":
                named = [value for value, kind in arguments if kind == "unquoted" and re.match(r"^\$\{[^}]*\}$", value)]
            else:
                named = values[:1]
            if any("$" in value for value in named):
                problems.append("%s writes a variable whose name CMake works out (%s): it could be one CMake runs a "
                                "program by - name it out" % (where, next(v for v in named if "$" in v)[:40]))
            for value in values:
                held = CMAKE_LINK_VARIABLES.get(value)
                if value.startswith("CMAKE_") and value not in CMAKE_WRITTEN and value not in CMAKE_LINK_VARIABLES:
                    problems.append("%s writes %s, a CMake variable CMakeLists.txt does not set (CMAKE_WRITTEN: %s): "
                                    "CMake runs a program by some (CMAKE_CXX_CLANG_TIDY with every compile, "
                                    "CMAKE_CXX_COMPILE_OBJECT for it), which could write a *-tests one - add it to "
                                    "CMAKE_WRITTEN with what it may be" % (where, value[:40], ", ".join(CMAKE_WRITTEN)))
                elif value in CMAKE_LINK_VARIABLES and not (lower == "find_library" and held is not None
                                                             and tuple(values) == held):
                    problems.append("%s writes %s, which target_link_libraries is given: only %s writes it "
                                    "(CMAKE_LINK_VARIABLES), as a flag in it would come after the linker's -o"
                                    % (where, value, "find_library(%s)" % " ".join(held) if held else "CMake"))
        if any(value.startswith("CMAKE_REQUIRED_") for value in values) and not (
                lower == "unset" and values == [CMAKE_REQUIRED[0]]
                or lower == "set" and tuple(values) == CMAKE_REQUIRED):
            problems.append("%s gives check_c_source_runs' compile other inputs than %s %s: a flag or a library "
                            "there is code its program runs" % ((where,) + CMAKE_REQUIRED))
        if lower == "include":
            if len(arguments) != 1 or arguments[0][1] != "unquoted" or values[0] not in CMAKE_INCLUDES:
                problems.append("%s line %d: include(%s) brings in CMake code build/check-source-tarball.py does not "
                                "read, from a file of the tree perhaps (a bare name no module has is a file beside "
                                "CMakeLists.txt), a tester among it: it loads %s alone - make the testers in %s"
                                % (CMAKELISTS, line, " ".join(values)[:60], ", ".join(CMAKE_INCLUDES), CMAKELISTS))
        elif lower == "find_package":
            if (not values or values[0] not in CMAKE_PACKAGES or worked_out(arguments)
                    or any(kind == "bracket" for _, kind in arguments)
                    or any(value.upper() in CMAKE_PACKAGE_PATHS for value in values)):
                problems.append("%s looks for a package build/check-source-tarball.py does not read, or where it does "
                                "not look (%s; no PATHS, HINTS, NAMES or CONFIGS): a package's configuration file "
                                "from the tree could make a tester" % (where, ", ".join(CMAKE_PACKAGES)))
        elif lower == "file":
            written = values[1:2] and re.split(r"[/\\]", values[1])[-1]
            if (not values or arguments[0][1] != "unquoted"
                    or values[0] not in CMAKE_FILE_READS + CMAKE_FILE_WRITES
                    or values[0] in CMAKE_FILE_WRITES and (arguments[1][1] == "bracket" or not written
                                                           or not CMAKE_LITERAL.match(written)
                                                           or written.endswith("-tests"))):
                problems.append("%s: file(%s) could copy, install or write a file, a *-tests program among them, "
                                "which linux-build.sh's find would run - file() may %s, and %s a file whose name "
                                "it gives out, not a *-tests" % (where, " ".join(values[:2])[:60],
                                                                 ", ".join(CMAKE_FILE_READS),
                                                                 " or ".join(CMAKE_FILE_WRITES)))
        elif lower == "configure_file":
            ends = [re.split(r"[/\\]", value)[-1] for value in values[:2]]
            if (len(values) < 2 or any(kind == "bracket" for _, kind in arguments[:2])
                    or any(not CMAKE_LITERAL.match(end) or end in (".", "..") or end.endswith("-tests")
                           for end in ends)):
                problems.append("%s line %d: configure_file() names a *-tests file, or one whose name it works out "
                                "or takes from its input (%s): it could write a program where linux-build.sh finds "
                                "and runs every *-tests program - name both files out, neither a *-tests"
                                % (CMAKELISTS, line, " ".join(values[:2])[:60]))
        elif lower == "add_custom_target":
            if any(value.upper() == "ALL" for value in values):
                problems.append("%s is built with every build (ALL): its commands could write a *-tests program, "
                                "which linux-build.sh's find would run" % where)
        elif lower == "install":
            if any(value.upper() in ("CODE", "SCRIPT") for value in values):
                problems.append("%s runs CMake code at install, which rpm runs before %%check: it could write a "
                                "*-tests program" % where)
    for number, text in enumerate(cmake_text.split("\n"), 1):
        named = CMAKE_NAMING_VARIABLES.search(text)
        if named and not text.lstrip().startswith("#"):
            problems.append("%s line %d: %s names or places every program, brings in CMake code from elsewhere or "
                            "points find_package() at a directory: the testers would not be the files the lists "
                            "name, or not all made here" % (CMAKELISTS, number, named.group(0)))
    return testers, problems


def cmake_testers(cmake_text):
    """The *-tests programs CMakeLists.txt makes, in its order."""
    return cmake_targets(cmake_text)[0]


def rules_testers(text):
    """debian/rules' TESTERS, or None where it has no such line."""
    lines = re.findall(r"^TESTERS\s*:?=(.*)$", text, re.M)
    return lines[0].split() if len(lines) == 1 else None


def spec_check(text):
    """The spec's %check section, or None."""
    check = re.search(r"^%check\s*$(.*?)^%(?!%)", text, re.M | re.S)
    return check.group(1).replace("\r\n", "\n") if check else None


def spec_testers(text):
    """The names the spec's %check loop runs, or None where it has no one such loop."""
    check = spec_check(text)
    if check is None:
        return None
    loops = re.findall(r"^for tester in ([^;\n]*); do$", check, re.M)
    return loops[0].split() if len(loops) == 1 else None


# What dh_auto_test runs: override_dh_auto_test's recipe - it runs the server and every
# name of $(TESTERS), each as a command of its own under set -e, skipping them only for
# nocheck and a cross build - so that a tester not run, a failure let through or a
# condition that runs nothing is refused.
RULES_TEST_RECIPE = (
    "override_dh_auto_test:\n"
    "ifeq (,$(filter nocheck,$(DEB_BUILD_OPTIONS)))\n"
    "ifeq ($(DEB_BUILD_GNU_TYPE),$(DEB_HOST_GNU_TYPE))\n"
    "\tset -e; cd $(BUILDDIR); echo \"== hmailserver --version\"; ./hmailserver --version; \\\n"
    "\t  for tester in $(TESTERS); do echo \"== $$tester\"; ./$$tester; done\n"
    "endif\n"
    "endif\n")
# And debian/rules as a whole, held line for line but for its comments, its blank lines
# and the names TESTERS gives: what make reads anywhere in it - another recipe that
# runs dh with DEB_BUILD_OPTIONS=nocheck or MAKEFLAGS=TESTERS=, rewrites debian/rules or
# the build directory, a second or target-specific TESTERS, .IGNORE, MAKEFLAGS, an eval, a
# conditional around the rule - can change what override_dh_auto_test runs, so a change
# to any rule of it is a change to this too.
RULES_HELD = (
    "export DEB_BUILD_MAINT_OPTIONS = hardening=+all optimize=-lto\n"
    "include /usr/share/dpkg/architecture.mk\n"
    "BUILDDIR := obj-$(DEB_HOST_GNU_TYPE)\n"
    "%:\n"
    "\tdh $@ --buildsystem=cmake+ninja --sourcedirectory=hmailserver/source/Server --builddirectory=$(BUILDDIR)\n"
    "override_dh_auto_configure:\n"
    "\tdh_auto_configure -- \\\n"
    "\t\t-DHM_LIVE_UPDATE=OFF \\\n"
    "\t\t\"-DHM_UPDATE_COMMAND=apt upgrade hmailserver\" \\\n"
    "\t\t-DHM_BOOST_STATIC=OFF \\\n"
    "\t\t-DCMAKE_JOB_POOLS=link=1 \\\n"
    "\t\t-DCMAKE_JOB_POOL_LINK=link\n"
    "TESTERS := <the testers>\n"
    + RULES_TEST_RECIPE +
    "override_dh_auto_install:\n"
    "\tdh_auto_install\n"
    "\tsed -i '1s|^#!/usr/bin/env python3$$|#!/usr/bin/python3|' debian/tmp/usr/bin/hmctl\n"
    "\t# CMake's FILES_MATCHING install of DBScripts leaves the one subdirectory that\n"
    "\t# holds no .sql behind, empty.\n"
    "\tfind debian/tmp/usr/share/hmailserver -type d -empty -delete\n"
    "override_dh_fixperms:\n"
    "\tdh_fixperms\n"
    "\tchmod 0644 debian/hmailserver/usr/bin/dh2048.pem\n"
    "override_dh_installsystemd:\n"
    "\tdh_installsystemd --no-start\n")
RULES_TESTERS = re.compile(r"^TESTERS := [a-z0-9 -]*$")
# The spec's %check, held line for line but for its comments: the server, then each
# tester the loop names, as a command of its own (rpmbuild runs the section under sh -e).
SPEC_CHECK_LINES = ("cd %{hm_builddir}", "echo \"== hmailserver --version\"", "./hmailserver --version",
                    None, "  echo \"== $tester\"", "  ./$tester", "done")
# And the spec's macros: only these defined (the build directory %check goes to among
# them, as the spec defines it), none taken away, no macro code run.
SPEC_MACROS = {"_vpath_srcdir": None, "__sourcedir": None, "__builddir": None, "__builder": None,
               "hm_builddir": ("_build", "%{__cmake_builddir}"), "hm_update_command": None,
               "quickjs_version": None, "_lto_cflags": None}
# rpm's conditionals, which it reads with blanks before them (spaces or tabs) and after
# expanding the line's macros, so with any run of % before them (%%if 0 is %if 0 to it);
# and one reached through a macro on the line ("%{?nil}%if 0", "%{?nil}%%if 0",
# "%{?quickjs_version:%%if 0}"), which rpm reads too and this refuses.
SPEC_IF = re.compile(r"^[ \t]*%+if(?:arch|narch|os|nos)?\b")
SPEC_ENDIF = re.compile(r"^[ \t]*%+endif\b")
SPEC_CONDITIONAL = re.compile(r"%+(?:if(?:arch|narch|os|nos)?|el(?:se|if\w*)|endif)\b")


def rules_problems(text):
    """debian/rules is RULES_HELD but for its comments, blank lines and the TESTERS list."""
    problems = []
    if "\r" in text:
        return ["%s has a carriage return: make would read it as part of a line (a rule's name, a recipe), and the "
                "file is checked out with line feeds alone on every platform (.gitattributes)" % DEBIAN_RULES]
    read, previous = [], ""
    for number, line in enumerate(text.split("\n"), 1):
        # make ignores a comment and a blank line, but not one a backslash joined to the line above.
        continued, previous = previous.endswith("\\"), line
        if not continued and line.startswith("#"):
            if line.endswith("\\"):
                problems.append("%s line %d is a comment ending in a backslash, which joins the next line to it: "
                                "make would read that line as comment" % (DEBIAN_RULES, number))
            continue
        if not continued and not line.strip():
            continue
        read.append((number, "TESTERS := <the testers>" if RULES_TESTERS.match(line) else line))
    held = RULES_HELD.rstrip("\n").split("\n")
    for index, want in enumerate(held):
        have = read[index] if index < len(read) else None
        if have is None or have[1] != want:
            problems.append("%s %s, where build/check-source-tarball.py holds %r there (RULES_HELD): debian/rules "
                            "is held line for line but for its comments and the testers TESTERS names, as what make "
                            "reads anywhere in it can change what override_dh_auto_test runs - another rule's "
                            "recipe running dh with DEB_BUILD_OPTIONS or MAKEFLAGS, a second TESTERS, a failure "
                            "ignored - change both together"
                            % (DEBIAN_RULES, ("line %d is %r" % have) if have else "ends", want[:70]))
            return problems
    if len(read) > len(held):
        problems.append("%s line %d is %r, after the last line build/check-source-tarball.py holds (RULES_HELD): "
                        "change both together" % ((DEBIAN_RULES,) + read[len(held)]))
    return problems


def spec_loop_problems(text):
    """The spec's one %check, outside any conditional, is SPEC_CHECK_LINES with the loop's list, its comments
    expand no macro, and the spec defines only SPEC_MACROS and runs no macro code."""
    problems = []
    if "\r" in text:
        problems.append("%s has a carriage return: rpm would read it as part of a line, and the file is checked out "
                        "with line feeds alone on every platform (.gitattributes)" % SPEC)
        text = text.replace("\r\n", "\n")
    check = spec_check(text)
    lines = [line for line in (check or "").split("\n") if line.strip() and not line.startswith("#")]
    shape = len(lines) == len(SPEC_CHECK_LINES) and all(
        want is None and re.match(r"^for tester in [a-z0-9 -]+; do$", have) or have == want
        for have, want in zip(lines, SPEC_CHECK_LINES))
    if len(re.findall(r"^[ \t]*%check\b", text, re.M)) != 1 or not shape:
        problems.append("%s's %%check is not, once, the section build/check-source-tarball.py holds "
                        "(SPEC_CHECK_LINES: the server, then ./$tester for each tester the loop names): a tester "
                        "it names may not be run, or its failure may not fail the build - change both together"
                        % SPEC)
    # rpm expands the macros of a comment: one whose text holds a line break (%{___build_post} ends in
    # "exit ${RPM_EC}") or a shell's output (%(...)) runs as the section's script, so %check's comments hold
    # none, a %% (an escaped %) aside.
    for line in (check or "").split("\n"):
        if line.startswith("#") and re.search(r"(?<!%)%(?!%)", line.replace("%%", "")):
            problems.append("%s's %%check has the comment %r, which expands a macro: rpm expands a comment's "
                            "macros too, and one that ends in a line break and an exit, as %%{___build_post} "
                            "does, ends the section there - write %%%% for a %%" % (SPEC, line[:60]))
    depth = 0
    for number, line in enumerate(text.split("\n"), 1):
        if SPEC_IF.match(line):
            depth += 1
        elif SPEC_ENDIF.match(line):
            depth -= 1
            if depth < 0:
                problems.append("%s line %d: an %%endif that closes no conditional this counted: rpm reads one "
                                "this did not (written after something else on its line, or through a macro), "
                                "which could leave %%check out" % (SPEC, number))
                depth = 0
        elif re.match(r"^[ \t]*%check\b", line) and depth:
            problems.append("%s line %d: %%check is inside a conditional, which can leave it out" % (SPEC, number))
        # One conditional on a line, its % run first after the blanks; any other is one rpm may read and this
        # does not count.
        conditionals = [found.start() for found in SPEC_CONDITIONAL.finditer(line)]
        if conditionals and not line.lstrip().startswith("#") and conditionals != [len(line) - len(line.lstrip(" \t"))]:
            problems.append("%s line %d has a conditional after something else on the line (%r): rpm expands the "
                            "line's macros first, so it reads \"%%{?nil}%%if 0\" and \"%%{?nil}%%%%if 0\" as "
                            "conditionals, which this would not count" % (SPEC, number, line[:60]))
        # Anywhere on the line, a comment's too: rpm expands the macros in a comment as well, a %define and a
        # %{lua:} among them. %{define:...} and %{global:...} are the same as %define and %global, with or
        # without blanks after the colon.
        for defined in list(re.finditer(r"(?<!%)%(?:global|define)\s+(\w+)\s*(.*?)\s*$", line)) + list(
                re.finditer(r"(?<!%)%\{(?:global|define):\s*(\w+)\s*([^}]*?)\s*\}", line)):
            if defined.group(1) not in SPEC_MACROS or (SPEC_MACROS[defined.group(1)] is not None
                                                       and defined.group(2) not in SPEC_MACROS[defined.group(1)]):
                problems.append("%s line %d defines %%%s as %r, which is none of the macros "
                                "build/check-source-tarball.py lets the spec define (SPEC_MACROS): a macro can change "
                                "what %%check runs or where" % (SPEC, number, defined.group(1), defined.group(2)[:40]))
        if re.search(r"(?<!%)%(?:\{(?:global|define):\s*(?!\w)|(?:global|define)\b(?!\s+\w))", line):
            problems.append("%s line %d defines a macro this cannot read the name of (%r)" % (SPEC, number,
                                                                                          line[:60]))
        if re.search(r"(?<!%)%(?:\{?(?:undefine|include|lua|expand|load|bcond\w*)\b|\()", line):
            problems.append("%s line %d takes a macro away, reads another file or runs macro or shell code "
                            "(%%undefine, %%include, %%lua, %%expand, %%load, %%bcond, %%(...)): what %%check runs "
                            "could be another section's than the one held" % (SPEC, number))
        if re.match(r"^\s*(?:BuildSystem|BuildOption)\b", line, re.I):
            problems.append("%s line %d: %s makes or changes rpm's own build sections, %%check among them"
                            % (SPEC, number, line.split(":")[0].strip()))
    if depth:
        problems.append("%s ends inside %d conditional(s) this counted: an %%if without its %%endif" % (SPEC, depth))
    return problems


def check_source_testers(read):
    """debian/rules' TESTERS and the spec's %check loop against each other and against
    the *-tests programs CMakeLists.txt makes, less SOURCE_TESTERS_LEFT_OUT."""
    made, problems = cmake_targets(read(CMAKELISTS))
    rules = rules_testers(read(DEBIAN_RULES))
    spec = spec_testers(read(SPEC))
    problems += rules_problems(read(DEBIAN_RULES)) + spec_loop_problems(read(SPEC))
    if not made:
        problems.append("%s makes no *-tests program (add_executable(<name>-tests ...)): the source packages' "
                        "testers cannot be held to it" % CMAKELISTS)
    if rules is None:
        problems.append("%s has no one TESTERS := line: the testers dh_auto_test runs cannot be read" % DEBIAN_RULES)
    if spec is None:
        problems.append("%s's %%check has no one 'for tester in ...; do' line: the testers it runs cannot be read" %
                        SPEC)
    if problems:
        return problems
    for where, names in ((DEBIAN_RULES + "'s TESTERS", rules), (SPEC + "'s %check", spec)):
        seen = set()
        for name in names:
            if name in seen:
                problems.append("%s names %s twice" % (where, name))
            seen.add(name)
            if name not in made:
                problems.append("%s runs %s, which %s does not make: a tester renamed or removed" %
                                (where, name, CMAKELISTS))
            elif name in SOURCE_TESTERS_LEFT_OUT:
                problems.append("%s runs %s, which SOURCE_TESTERS_LEFT_OUT in build/check-source-tarball.py leaves "
                                "out (%s): run it or leave it out, not both" %
                                (where, name, SOURCE_TESTERS_LEFT_OUT[name]))
    if rules != spec:
        problems.append("%s's TESTERS and %s's %%check do not run the same testers in the same order: the PPA's "
                        "build and Copr's and the Build Service's run different checks (only in TESTERS: %s; only in "
                        "%%check: %s)" % (DEBIAN_RULES, SPEC, ", ".join(n for n in rules if n not in spec) or "none",
                                          ", ".join(n for n in spec if n not in rules) or "none"))
    for name in made:
        if name not in rules and name not in spec and name not in SOURCE_TESTERS_LEFT_OUT:
            problems.append("%s makes %s, and neither %s's TESTERS nor %s's %%check runs it: add it to both, or to "
                            "SOURCE_TESTERS_LEFT_OUT in build/check-source-tarball.py with the reason a distribution's "
                            "build cannot run it" % (CMAKELISTS, name, DEBIAN_RULES, SPEC))
    for name in sorted(SOURCE_TESTERS_LEFT_OUT):
        if name not in made:
            problems.append("SOURCE_TESTERS_LEFT_OUT in build/check-source-tarball.py leaves out %s, which %s does not "
                            "make: a tester renamed or removed" % (name, CMAKELISTS))
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
        "hmailserver/docs/Licenses/License - Feather icons.txt": "MIT",
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
            "hmailserver/docs/Licenses/License - Feather icons.txt": b"MIT",
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

    # The source packages' testers, over the tree's own CMakeLists.txt, debian/rules
    # and spec, each changed in a copy.
    texts = {path: read_file(ROOT, path) for path in (CMAKELISTS, DEBIAN_RULES, SPEC)}

    def testers(cmake=None, rules=None, spec=None):
        given = {CMAKELISTS: cmake, DEBIAN_RULES: rules, SPEC: spec}
        return check_source_testers(lambda p: given[p] if given.get(p) is not None else texts[p])

    def changed(path, old, new):
        if old not in texts[path]:
            failures.append("the self-test's %r is no longer in %s: the case below cannot be made" % (old, path))
            return texts[path]
        return texts[path].replace(old, new, 1)

    rules_line = re.search(r"^TESTERS\s*:?=[^\r\n]*", texts[DEBIAN_RULES], re.M).group(0)
    spec_line = re.search(r"^for tester in [^;\r\n]*; do(?=\r?$)", texts[SPEC], re.M).group(0)
    new_target = ("add_executable(cache-tests ${SERVER_DIR}/platform/cache_tests_main.cpp)\n"
                  "target_link_libraries(cache-tests PRIVATE hmailserver_core)\n")
    expect("the source packages' testers as the tree has them", testers(), None)
    expect("a tester CMakeLists.txt makes and neither list runs",
           testers(cmake=texts[CMAKELISTS] + "\n" + new_target), "makes cache-tests, and neither")
    expect("the same tester added to both lists", testers(
        cmake=texts[CMAKELISTS] + "\n" + new_target,
        rules=changed(DEBIAN_RULES, rules_line, rules_line + " cache-tests"),
        spec=changed(SPEC, spec_line, spec_line.replace("; do", " cache-tests; do"))), None)
    expect("a tester the spec runs and debian/rules does not",
           testers(rules=changed(DEBIAN_RULES, " acme-tests", "")), "only in %check: acme-tests")
    expect("the two lists in another order",
           testers(spec=changed(SPEC, " idna-tests acme-tests ", " acme-tests idna-tests ")), "in the same order")
    expect("a tester left out and run",
           testers(rules=changed(DEBIAN_RULES, rules_line, rules_line + " root-command-tests"),
                   spec=changed(SPEC, spec_line, spec_line.replace("; do", " root-command-tests; do"))),
           "runs root-command-tests, which SOURCE_TESTERS_LEFT_OUT")
    expect("a tester run twice",
           testers(rules=changed(DEBIAN_RULES, rules_line, rules_line + " dane-tests")), "names dane-tests twice")
    expect("a tester CMakeLists.txt no longer makes",
           testers(cmake=changed(CMAKELISTS, "add_executable(acme-tests ", "add_executable(acme-checks ")),
           "runs acme-tests, which %s does not make" % CMAKELISTS)
    expect("a tester left out that CMakeLists.txt no longer makes",
           testers(cmake=changed(CMAKELISTS, "add_executable(root-command-tests ", "add_executable(root-command ")),
           "leaves out root-command-tests, which")
    expect("debian/rules without its TESTERS line",
           testers(rules=changed(DEBIAN_RULES, rules_line, "# " + rules_line)), "has no one TESTERS := line")
    expect("the spec's loop outside %check",
           testers(spec=changed(SPEC, "\n%check", "\n%check_moved")), "has no one 'for tester in")
    # add_executable as CMake reads it, and a name it would have to work out.
    expect("a tester made by ADD_EXECUTABLE, which neither list runs",
           testers(cmake=texts[CMAKELISTS] + "\n" + new_target.replace("add_executable(", "ADD_EXECUTABLE(")),
           "makes cache-tests, and neither")
    expect("a tester made with a space before add_executable's bracket, which neither list runs",
           testers(cmake=texts[CMAKELISTS] + "\n" + new_target.replace("add_executable(", "add_executable (")),
           "makes cache-tests, and neither")
    expect("testers made in a foreach",
           testers(cmake=texts[CMAKELISTS] + "\nforeach(name cache)\n   add_executable(${name}-tests "
                   "${SERVER_DIR}/platform/${name}_tests_main.cpp)\nendforeach()\n"), "is not named by a literal")
    expect("a tester whose program is named otherwise than its target",
           testers(cmake=texts[CMAKELISTS] + "\nadd_executable(cache-check ${SERVER_DIR}/platform/cache_tests_main.cpp)\n"
                   "set_target_properties(cache-check PROPERTIES OUTPUT_NAME cache-tests)\n"), "gives cache-check OUTPUT_NAME cache-tests")
    expect("targets brought in from another directory",
           testers(cmake=texts[CMAKELISTS] + "\nadd_subdirectory(testers)\n"), "add_subdirectory() makes targets")
    # The two loops: each runs every name it is given, and a failure fails the build.
    expect("debian/rules' loop over another variable",
           testers(rules=changed(DEBIAN_RULES, "for tester in $(TESTERS); do", "for tester in $(OTHER_TESTERS); do")),
           "(RULES_HELD)")
    expect("debian/rules' loop without set -e",
           testers(rules=changed(DEBIAN_RULES, "set -e; cd $(BUILDDIR);", "cd $(BUILDDIR);")),
           "(RULES_HELD)")
    expect("debian/rules' loop that does not run its tester",
           testers(rules=changed(DEBIAN_RULES, "; ./$$tester; done", "; : ./$$tester; done")),
           "(RULES_HELD)")
    expect("debian/rules' loop whose tester's failure is let through",
           testers(rules=changed(DEBIAN_RULES, "; ./$$tester; done", "; ./$$tester || true; done")),
           "(RULES_HELD)")
    expect("the spec's loop that does not run its tester",
           testers(spec=changed(SPEC, "  ./$tester", "  : ./$tester")), "is not, once, the section")
    expect("the spec's loop whose tester's failure is let through",
           testers(spec=changed(SPEC, "  ./$tester", "  ./$tester || :")), "is not, once, the section")
    # A *-tests program CMake makes by a command this does not read as add_executable, each of which linux-build.sh's
    # find would run and the lists would not; and what make and rpm read outside the held recipe and section.
    cache = "add_executable(cache-check ${SERVER_DIR}/platform/cache_tests_main.cpp)\n"
    for label, added, needle in (
            ("a tester's name given by set_property", cache + "set_property(TARGET cache-check PROPERTY OUTPUT_NAME "
             "cache-tests)\n", "PROPERTY OUTPUT_NAME) names or places"),
            ("a tester's name given by RUNTIME_OUTPUT_NAME", cache + "set_target_properties(cache-check PROPERTIES "
             "RUNTIME_OUTPUT_NAME cache-tests)\n", "gives cache-check RUNTIME_OUTPUT_NAME cache-tests"),
            ("a tester's name given by SUFFIX", "add_executable(cache ${SERVER_DIR}/platform/cache_tests_main.cpp)\n"
             "set_target_properties(cache PROPERTIES SUFFIX -tests)\n", "gives cache SUFFIX -tests"),
            ("a tester placed in another directory", "set_target_properties(acme-tests PROPERTIES "
             "RUNTIME_OUTPUT_DIRECTORY elsewhere)\n", "RUNTIME_OUTPUT_DIRECTORY elsewhere"),
            ("every program placed elsewhere", "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY bin)\n",
             "CMAKE_RUNTIME_OUTPUT_DIRECTORY names or places"),
            ("a tester made by cmake_language(CALL)", "cmake_language(CALL add_executable cache-tests "
             "${SERVER_DIR}/platform/cache_tests_main.cpp)\n", "cmake_language() makes targets"),
            ("a tester made by add_executable's original", "_add_executable(cache-tests "
             "${SERVER_DIR}/platform/cache_tests_main.cpp)\n", "_add_executable() makes targets"),
            ("add_executable defined over", "macro(add_executable name)\n   _add_executable(${name}-tests ${ARGN})\n"
             "endmacro()\n", "macro() makes targets"),
            ("a tester made in a function", "function(tester name)\n   add_executable(${name} ${ARGN})\n"
             "endfunction()\n", "function() makes targets"),
            ("testers from another directory by subdirs", "subdirs(testers)\n", "subdirs() makes targets"),
            ("a module of the tree through CMAKE_MODULE_PATH", "list(APPEND CMAKE_MODULE_PATH ${SERVER_DIR}/cmake)\n"
             "include(Testers)\n", "CMAKE_MODULE_PATH names or places"),
            ("testers from FetchContent", "include(FetchContent)\nFetchContent_Declare(testers SOURCE_DIR "
             "${SERVER_DIR}/testers)\nFetchContent_MakeAvailable(testers)\n", "FetchContent_Declare() makes targets"),
            ("a *-tests program written by configure_file", "configure_file(${SERVER_DIR}/t.sh cache-tests "
             "FILE_PERMISSIONS OWNER_READ OWNER_EXECUTE)\n", "configure_file() names a *-tests file")):
        expect(label, testers(cmake=texts[CMAKELISTS] + "\n" + added), needle)
    rules_text = texts[DEBIAN_RULES].replace("\r\n", "\n")
    spec_text = texts[SPEC].replace("\r\n", "\n")
    recipe_line = "\tset -e; cd $(BUILDDIR); echo"
    for label, rules, needle in (
            ("TESTERS emptied for the recipe alone", rules_text + "\noverride_dh_auto_test: TESTERS :=\n",
             "(RULES_HELD)"),
            ("TESTERS assigned again, indented by a space", rules_text.replace(rules_line, rules_line + "\n "
             + "TESTERS := spf-tests", 1), "(RULES_HELD)"),
            ("TESTERS assigned again, indented by a tab outside a rule", rules_text.replace(
                "BUILDDIR := ", "\tTESTERS := spf-tests\nBUILDDIR := ", 1), "(RULES_HELD)"),
            ("TESTERS assigned again", rules_text.replace(rules_line, rules_line + "\nTESTERS := spf-tests", 1),
             "(RULES_HELD)"),
            ("TESTERS assigned by an eval in another recipe", rules_text.replace(
                "\tdh_auto_install\n", "\tdh_auto_install $(eval TESTERS := spf-tests)\n", 1), "(RULES_HELD)"),
            ("the recipe's failures ignored by .IGNORE", rules_text + "\n.IGNORE: override_dh_auto_test\n",
             "(RULES_HELD)"),
            ("every failure ignored by MAKEFLAGS", rules_text.replace("BUILDDIR := ", "MAKEFLAGS += -i\nBUILDDIR := ",
                                                                       1), "(RULES_HELD)"),
            ("set +e after set -e", rules_text.replace(recipe_line, "\tset -e; set +e; cd $(BUILDDIR); echo", 1),
             "(RULES_HELD)"),
            ("exit 0 before the loop", rules_text.replace("./hmailserver --version; \\", "./hmailserver --version; "
                                                           "exit 0; \\", 1), "(RULES_HELD)"),
            ("the testers run only for another condition", rules_text.replace(
                "ifeq ($(DEB_BUILD_GNU_TYPE),$(DEB_HOST_GNU_TYPE))", "ifneq ($(DEB_BUILD_GNU_TYPE),$(DEB_HOST_GNU_TYPE))",
                1), "(RULES_HELD)"),
            ("the rule inside a conditional", rules_text.replace("override_dh_auto_test:\n", "ifeq (a,b)\n"
                                                                  "override_dh_auto_test:\n", 1).replace(
                "\n\n# hmctl", "endif\n\n# hmctl", 1), "(RULES_HELD)")):
        expect("debian/rules: " + label, testers(rules=rules), needle)
    loop_line = spec_line + "\n"
    for label, spec, needle in (
            ("continue above ./$tester", spec_text.replace("  ./$tester\n", "  continue\n  ./$tester\n", 1),
             "is not, once, the section"),
            ("set +e before the loop", spec_text.replace(loop_line, "set +e\n" + loop_line, 1),
             "is not, once, the section"),
            ("exit 0 before the loop", spec_text.replace(loop_line, "exit 0\n" + loop_line, 1),
             "is not, once, the section"),
            ("the build directory that runs nothing", spec_text.replace("%global hm_builddir _build\n",
                                                                        "%global hm_builddir _build; exit 0\n", 1),
             "defines %hm_builddir"),
            ("a check macro defined", spec_text.replace("%global quickjs_version", "%define __spec_check_pre exit 0\n"
                                                        "%global quickjs_version", 1), "defines %__spec_check_pre"),
            ("%check inside a conditional", spec_text.replace("\n%check\n", "\n%if 0\n%check\n", 1).replace(
                "\ndone\n", "\ndone\n%endif\n", 1), "inside a conditional"),
            ("a macro defined by Lua", spec_text.replace("%global quickjs_version", "%{lua: rpm.define(\"hm_builddir "
                                                         "/nowhere\")}\n%global quickjs_version", 1),
             "runs macro or shell code"),
            # rpm expands the macros of a comment too.
            ("a check macro defined in a comment", spec_text.replace("\n%check\n", "\n%check\n# %define "
                                                                     "__spec_check_pre exit 0\n", 1),
             "defines %__spec_check_pre"),
            ("a check macro defined as %{define:...}", spec_text.replace("%global quickjs_version", "%{define:"
                                                                         "__spec_check_pre exit 0}\n%global "
                                                                         "quickjs_version", 1),
             "defines %__spec_check_pre"),
            ("Lua in a comment", spec_text.replace("\n%check\n", "\n%check\n# %{lua: rpm.define(\"hm_builddir "
                                                   "/nowhere\")}\n", 1), "runs macro or shell code")):
        expect("the spec: " + label, testers(spec=spec), needle)
    # More ways CMake makes or writes an executable *-tests file in the top build directory, or could: a project
    # in the srcpkg-debian13 job's image (cmake 3.31.6) configured with -DCMAKE_BUILD_TYPE=Release made one by
    # each of the two quoted keys, the variable key, RELEASE_POSTFIX, try_compile, exec_program, file(COPY),
    # find_package() from the tree and include() of a bare name; the rest are what this does not read.
    hidden = "add_executable(hidden ${SERVER_DIR}/platform/cache_tests_main.cpp)\n"
    for label, added, needle in (
            ("a tester's name given by a quoted OUTPUT_NAME", hidden + "set_target_properties(hidden PROPERTIES "
             "\"OUTPUT_NAME\" hidden-tests)\n", "gives hidden OUTPUT_NAME hidden-tests"),
            ("a tester's name given by set_property and a quoted OUTPUT_NAME", hidden + "set_property(TARGET hidden "
             "PROPERTY \"OUTPUT_NAME\" hidden-tests)\n", "PROPERTY OUTPUT_NAME) names or places"),
            ("a tester's name given by a property named by a variable", "set(HM_KEY OUTPUT_NAME)\n" + hidden
             + "set_target_properties(hidden PROPERTIES ${HM_KEY} hidden-tests)\n", "an argument CMake works out"),
            ("a tester's name given by a property in a bracket argument", hidden + "set_target_properties(hidden "
             "PROPERTIES [[OUTPUT_NAME]] hidden-tests)\n", "an argument CMake works out"),
            ("a tester's name given by RELEASE_POSTFIX", hidden + "set_target_properties(hidden PROPERTIES "
             "RELEASE_POSTFIX -tests)\n", "gives hidden RELEASE_POSTFIX -tests"),
            ("a tester copied out of try_compile", "try_compile(HM_RESULT ${CMAKE_BINARY_DIR}/tc SOURCES "
             "${SERVER_DIR}/x.c COPY_FILE ${CMAKE_BINARY_DIR}/hidden-tests)\n", "try_compile(HM_RESULT"),
            ("a tester copied by exec_program", "exec_program(cp ARGS ${SERVER_DIR}/scripts/hidden "
             "${CMAKE_BINARY_DIR}/hidden-tests)\n", "exec_program(cp"),
            ("a directory holding a tester copied by file(COPY)", "file(COPY ${SERVER_DIR}/scripts/ DESTINATION "
             "${CMAKE_BINARY_DIR})\n", "file(COPY"),
            ("a tester written by file(WRITE)", "file(WRITE ${CMAKE_BINARY_DIR}/hidden-tests \"#!/bin/sh\")\n",
             "file(WRITE"),
            ("a package's configuration from the tree", "find_package(HmHidden CONFIG REQUIRED PATHS "
             "${SERVER_DIR}/cmake NO_DEFAULT_PATH)\n", "looks for a package"),
            ("a package the tree uses, from a directory of the tree", "find_package(Boost CONFIG PATHS "
             "${SERVER_DIR}/cmake)\n", "looks for a package"),
            ("a package's directory set", "set(Boost_DIR ${SERVER_DIR}/cmake)\n", "Boost_DIR names or places"),
            ("a directory put first on the path find_package() searches", "set(ENV{PATH} \"${SERVER_DIR}/x/bin:"
             "$ENV{PATH}\")\n", "ENV{ names or places"),
            ("a file of the tree brought in by include() and its path", "include(${SERVER_DIR}/cmake/x.cmake)\n",
             "include(${SERVER_DIR}/cmake/x.cmake) brings in"),
            ("a file of the tree brought in by include() and a bare name", "include(HmHiddenTesters)\n",
             "include(HmHiddenTesters) brings in"),
            ("a tester written by configure_file under its input's name", "configure_file(${SERVER_DIR}/t/"
             "hidden-tests .)\n", "configure_file() names a *-tests file"),
            ("a file written by configure_file under a name it works out", "configure_file(${SERVER_DIR}/t.sh "
             "${CMAKE_BINARY_DIR}/${HM_NAME})\n", "configure_file() names a *-tests file"),
            ("a custom target built with every build", "add_custom_target(hidden ALL COMMAND cp a b)\n",
             "built with every build"),
            ("CMake code run at install", "install(CODE \"message(x)\")\n", "runs CMake code at install"),
            ("a command this does not read", "add_custom_command(OUTPUT x COMMAND cp a b)\n",
             "a command build/check-source-tarball.py does not read"),
            ("every module made a program", "set(CMAKE_SHARED_MODULE_PREFIX \"\")\n",
             "CMAKE_SHARED_MODULE_PREFIX names or places"),
            ("a bracket never closed", "message([=[ x ]])\n", "CMake would read it otherwise"),
            # The toolchain: a flag after the linker's own -o names the output (made, as above), and the rest
            # are what this does not read.
            ("a tester named by the linker's -o", hidden + "target_link_libraries(hidden PRIVATE -ohidden-tests)\n",
             "gives the linker -ohidden-tests"),
            ("a compiler plugin from the tree", "add_compile_options(-fplugin=${SERVER_DIR}/x.so)\n",
             "a flag build/check-source-tarball.py does not read"),
            ("a target's link options", hidden + "set_target_properties(hidden PROPERTIES LINK_OPTIONS "
             "-ohidden-tests)\n", "LINK_OPTIONS, a target property build/check-source-tarball.py does not read"),
            ("the directory's link options", "set_property(DIRECTORY APPEND PROPERTY LINK_OPTIONS -ohidden-tests)\n",
             "sets a property build/check-source-tarball.py does not read"),
            ("a launcher for every compile", "set(CMAKE_C_COMPILER_LAUNCHER ${SERVER_DIR}/x.sh)\n",
             "CMAKE_C_COMPILER_LAUNCHER names or places"),
            ("another program run while CMake configures", "include(CheckCSourceRuns)\ncheck_c_source_runs([=[\n"
             "int main(void) { return 0; }\n]=] HM_ICONV_CONVERTS_UTF7)\n", "compiles and runs a program"),
            ("that program given a library of the tree", "set(CMAKE_REQUIRED_LIBRARIES ${SERVER_DIR}/x.a)\n",
             "gives check_c_source_runs' compile other inputs"),
            # Five more, each of which made an executable *-tests program in the top build directory in that
            # image: a carriage return alone, which CMake takes for a space between arguments; a flag after it in
            # target_link_libraries; a flag given through a variable, and -o with no word boundary after it; a
            # tool CMake runs with every compile; and a second command in the compile rule.
            ("a key and its value split by a carriage return alone", hidden + "set_target_properties(hidden "
             "PROPERTIES OUTPUT_NAME\rhidden-tests)\n", "has a carriage return that is not followed by a line feed"),
            ("a flag after a carriage return alone in target_link_libraries", hidden + "target_link_libraries("
             "hidden PRIVATE m\r-ohidden-tests)\n", "has a carriage return that is not followed by a line feed"),
            ("a flag given to target_link_libraries through a variable", "set(HM_PROBE_FLAG m)\n" + hidden
             + "target_link_libraries(hidden PRIVATE ${HM_PROBE_FLAG})\n", "gives the linker ${HM_PROBE_FLAG}"),
            ("-o with a name right after it, through a variable", "set(HM_PROBE_FLAG -ohidden3-tests)\n",
             "-o names or places"),
            ("a link variable written by set()", "set(LDAP_LIBRARY m)\n",
             "writes LDAP_LIBRARY, which target_link_libraries is given"),
            ("a link variable found elsewhere", "find_library(LBER_LIBRARY NAMES lber PATHS ${SERVER_DIR})\n",
             "writes LBER_LIBRARY, which target_link_libraries is given"),
            ("a tool run with every compile", "set(CMAKE_CXX_CLANG_TIDY sh -c \"cp /bin/true hidden5-tests\")\n",
             "writes CMAKE_CXX_CLANG_TIDY, a CMake variable"),
            ("a second command in the compile rule", "list(APPEND CMAKE_CXX_COMPILE_OBJECT \"cp /bin/true "
             "hidden6-tests\")\n", "writes CMAKE_CXX_COMPILE_OBJECT, a CMake variable"),
            ("a CMake variable written by string()", "string(APPEND CMAKE_CXX_COMPILE_OBJECT \" && true\")\n",
             "writes CMAKE_CXX_COMPILE_OBJECT, a CMake variable"),
            ("a variable written by a name worked out", "set(HM_NAME CMAKE_CXX_CLANG_TIDY)\nset(${HM_NAME} true)\n",
             "writes a variable whose name CMake works out"),
            ("a variable written by string() by a name worked out", "set(HM_NAME CMAKE_CXX_CLANG_TIDY)\n"
             "string(TOLOWER true ${HM_NAME})\n", "writes a variable whose name CMake works out")):
        expect(label, testers(cmake=texts[CMAKELISTS] + "\n" + added), needle)
    # debian/rules, held whole: what the other rules' recipes run, and comments make joins on.
    for label, rules, needle in (
            ("dh told nocheck in the % rule's recipe", rules_text.replace("\tdh $@ ", "\tDEB_BUILD_OPTIONS=nocheck "
                                                                          "dh $@ ", 1), "(RULES_HELD)"),
            ("dh given an empty TESTERS in the % rule's recipe", rules_text.replace("\tdh $@ ", "\tMAKEFLAGS=TESTERS= "
                                                                                  "dh $@ ", 1), "(RULES_HELD)"),
            ("debian/rules rewritten by another recipe", rules_text.replace(
                "\t\t-DCMAKE_JOB_POOL_LINK=link\n", "\t\t-DCMAKE_JOB_POOL_LINK=link\n\tsed -i 's/^TESTERS :=.*/"
                "TESTERS :=/' debian/rules\n", 1), "(RULES_HELD)"),
            ("a comment ending in a backslash above TESTERS", rules_text.replace(rules_line, "# note \\\n"
                                                                                 + rules_line, 1),
             "a comment ending in a backslash"),
            ("a comment joined to the test recipe's first line", rules_text.replace(
                "./hmailserver --version; \\\n", "./hmailserver --version; \\\n# note\n", 1), "(RULES_HELD)"),
            ("a carriage return", rules_text.replace("override_dh_auto_test:\n", "override_dh_auto_test:\r\n", 1),
             "has a carriage return")):
        expect("debian/rules: " + label, testers(rules=rules), needle)
    # The spec: conditionals and definitions as rpm reads them (rpm 6.0.2 in the srcpkg-fedora44 job's image
    # left %check out, exit 0, for each of the first five, and for a comment of %check that expands
    # %{___build_post} or runs the shell). The %endif closes the indented %if after the section.
    check_end = "\ndone\n"
    for label, spec, needle in (
            ("%check inside an %if indented by spaces", spec_text.replace("\n%check\n", "\n  %if 0\n%check\n", 1)
             .replace(check_end, check_end + "%endif\n", 1), "inside a conditional"),
            ("%check inside an %if indented by a tab", spec_text.replace("\n%check\n", "\n\t%if 0\n%check\n", 1)
             .replace(check_end, check_end + "%endif\n", 1), "inside a conditional"),
            ("%check inside an %if reached through a macro", spec_text.replace(
                "\n%check\n", "\n%{?nil}%if 0\n%check\n", 1).replace(check_end, check_end + "%endif\n", 1),
             "a conditional after something else"),
            ("a check macro defined as %{define: ...}", spec_text.replace(
                "%global quickjs_version", "%{define: __spec_check_pre exit 0}\n%global quickjs_version", 1),
             "defines %__spec_check_pre"),
            ("a check macro defined as %{global:\\t...}", spec_text.replace(
                "%global quickjs_version", "%{global:\t__spec_check_pre exit 0}\n%global quickjs_version", 1),
             "defines %__spec_check_pre"),
            ("a comment of %check that ends the section", spec_text.replace(
                "\ncd %{hm_builddir}\n", "\n# %{___build_post}\ncd %{hm_builddir}\n", 1), "which expands a macro"),
            ("a comment of %check that runs the shell", spec_text.replace(
                "\ncd %{hm_builddir}\n", "\n# %(printf 'exit 0')\ncd %{hm_builddir}\n", 1), "which expands a macro"),
            ("a macro defined by name worked out", spec_text.replace(
                "%global quickjs_version", "%global %{?hm_x}__spec_check_pre exit 0\n%global quickjs_version", 1),
             "defines a macro this cannot read the name of"),
            ("rpm's own build sections", spec_text.replace("\n%description\n", "\nBuildSystem: cmake\n%description\n",
                                                            1), "makes or changes rpm's own build sections"),
            ("a shell command run while rpm reads the spec", spec_text.replace("1%{?dist}\n", "1%{?dist}%(true)\n", 1),
             "runs macro or shell code"),
            ("a carriage return", spec_text.replace("\n%check\n", "\n%check\r\n", 1), "has a carriage return"),
            # rpm expands a line's macros before it reads its conditional, so a %% before one is a %: each of these
            # left %check out with rpmbuild exit 0 in that image, the %endif after the section a plain one.
            ("%check inside a %%if", spec_text.replace("\n%check\n", "\n%%if 0\n%check\n", 1)
             .replace(check_end, check_end + "%endif\n", 1), "inside a conditional"),
            ("%check inside a %%if reached through a macro", spec_text.replace(
                "\n%check\n", "\n%{?nil}%%if 0\n%check\n", 1).replace(check_end, check_end + "%endif\n", 1),
             "a conditional after something else"),
            ("%check inside a %%if in a macro's text", spec_text.replace(
                "\n%check\n", "\n%{?quickjs_version:%%if 0}\n%check\n", 1).replace(check_end, check_end + "%endif\n", 1),
             "a conditional after something else"),
            ("an %endif that closes nothing counted", spec_text.replace(check_end, check_end + "%endif\n", 1),
             "an %endif that closes no conditional"),
            ("an %if never closed", spec_text.replace(check_end, check_end + "%if 0\n", 1),
             "ends inside 1 conditional")):
        if spec == spec_text:
            failures.append("the spec: %s: the case cannot be made from the tree's spec" % label)
            continue
        expect("the spec: " + label, testers(spec=spec), needle)

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
        tester_problems = check_source_testers(lambda p: read_file(ROOT, p))
    except Problem as e:
        print(str(e))
        return 2
    if problems or tester_problems:
        for what, found in ((EXCLUSIONS, problems), ("the source packages' testers", tester_problems)):
            if found:
                print("%s: %d problem(s)" % (what, len(found)))
                for problem in found:
                    print("  " + problem)
        return 1
    left = sum(1 for p in tracked if excluded_by(p, entries))
    print("%s: %d lines leave out %d of the %d tracked files; none is read by the Linux build, every file of the "
          "Windows-only trees is accounted for, and every licence the tarball must carry stays." %
          (EXCLUSIONS, len(entries), left, len(tracked)))
    made = cmake_testers(read_file(ROOT, CMAKELISTS))
    print("The source packages' builds run %d of the %d tester programs CMakeLists.txt makes, debian/rules and the "
          "spec alike; the other %d are left out, each with its reason." %
          (len(made) - len(SOURCE_TESTERS_LEFT_OUT), len(made), len(SOURCE_TESTERS_LEFT_OUT)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
