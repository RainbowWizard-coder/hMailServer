#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The licence fields of the Linux packages name what is compiled into the server, or the project's licence alone.

Nine fields, in the files the Linux packages, the snap, the container image and the Helm
chart are made from, say under which licences each goes out, where a package manager, a
store, a registry, a distribution and an auditor read it. This holds all nine.

Five of them (FIELDS) name the licences of what is compiled into the server: the
project's own .rpm (CPACK_RPM_PACKAGE_LICENSE in CMakeLists.txt), the RPM spec's License,
the APKBUILD's license=, the PKGBUILD's license= and the Nix package's meta.license. Each
must name the server's own licence and the licence of every third-party component
compiled into the server that an identifier names, and nothing else:
hmailserver/docs/PackagesRpm.md, "The licences", says so of the tag. Until 6.3.7 four of
them named neither licence of the two data sets idn-uts46 compiled in - Unicode's data for
UTS 46 (Unicode-3.0) and HTML's list of named character references (BSD-3-Clause) - and
the PKGBUILD named the AGPL alone (release review 6.3.7, B-DOC-1).

The other four (OWN_LICENCE_ONLY) name the project's own licence alone, each for the
reason given there: the snap's license, the container image's
org.opencontainers.image.licenses and io.artifacthub.package.license labels, and the Helm
chart's artifacthub.io/license. The snap and the image carry the .deb's files, and with
them its copyright file and the notices of what is compiled into the server; the chart
carries no binary. Each must be there once and say AGPL-3.0-or-later, so that one made to
name more is decided here - moved to FIELDS and held to what is compiled in - rather than
left short of it.

And no other line of the files the scan reads is a licence field, nor is anything on the
line of one of the nine after the field read there: a field this holds has its line to
itself, for a Dockerfile's LABEL takes several key=value pairs on a line, and the image
gets the last of a key. It reads the files of snap/, nix/ and platform/packaging/
(SCANNED_DIRECTORIES), flake.nix, the CMakeLists and build/ci/linux-image.sh, which gives
kaniko the image's labels that change with each build as --label flags (SCANNED_FILES) -
those git tracks, where there is git - by the kind of file (FIELD_LINES), and takes for a
licence field a line, not a comment, that

  in an RPM spec             has a License tag, in any case, with or without a space
                             before its colon;
  in an APKBUILD or a        assigns license - license=, license+= or an element's
  PKGBUILD                   license[...]=, anywhere on the line, after && or a declare;
  in a Nix file              binds a licence - license =, meta.license =, the name in
                             quotes - or inherits license;
  in a CMake file            names a CPack variable of a package's licence,
                             CPACK_*PACKAGE_LICENSE*, other than in ${...}: set(),
                             string(), list() and any other command that names the
                             variable it changes, whatever the case of the command;
  in YAML                    has a licence key - license, licence, licenses or licences,
                             prefixed as artifacthub.io/license is, in quotes or not - at
                             its start, after a sequence's dashes, or in a flow after its
                             {, its [ or a comma; or names one of the image's two licence
                             labels;
  in a Dockerfile or a       names one of the image's two licence labels;
  Containerfile
  in AppStream's XML         has <project_license> or <metadata_license>;
  in a shell script (*.sh)   sets one of the image's two licence labels as KEY=VALUE, its
                             key in quotes or not, or by the older LABEL KEY VALUE, or
                             writes AppStream's licence.

A field added there fails until it is in one list or the other. The scan reads lines, not
the languages: a field set another way - by a name the file builds (a set() of ${name} in
CMake, or cmake_parse_arguments' prefix), a macro, or a command that reads into one, such
as bash's read - can pass it, and so can a field in a file it does not read: a build reads
more than these, the CI files and the other scripts under build/ among them. As first
written, this checked the five alone and said they were every licence field of the Linux
packages, and the other four were licence fields that nothing held (release review 6.3.7,
the check of RB-2); the scan that came of that read neither linux-image.sh nor a shell
script, while the CHANGELOG said it read every file the packages are made from (the
re-check of RB-2); and it skipped the line of a field it held whole, so that another
licence label there passed and the image got it, and it read a CMake file's set() alone, an
APKBUILD's or a PKGBUILD's license= and += at a line's start alone, a YAML key at a line's
start or after one dash alone, a Nix licence bound by its bare name alone and a shell
script's label by its bare key alone (the check of RB-2's third round).

What is compiled in is decided component by component against build/asset-sbom.py's
SOURCE_COMPONENTS, the third-party parts of the source tarball as
hmailserver/docs/SourceTarball.md declares them, read here without running that file:
each of its components must be in COMPILED_IN or NOT_COMPILED_IN below, so that one added
there is decided here before this passes, and a component SOURCE_COMPONENTS gives an SPDX
identifier must have that identifier here. Then each of the five must name:

  AGPL-3.0-or-later              the server's own;
  each COMPILED_IN identifier    by its SPDX identifier; one SPDX has none for has the
                                 RPM fields' name of it alone, Fedora's (CStdString's
                                 LicenseRef-Fedora-UltraPermissive), and the others leave
                                 it to the copyright file;
  BSL-1.0                        where that build links Boost statically - the CMake
                                 option HM_BOOST_STATIC, ON unless the file passes
                                 -DHM_BOOST_STATIC=OFF - as the project's .rpm says of
                                 Boost; never where it links the distribution's shared
                                 Boost.

The SPDX fields are read as an AND of identifiers, which is all they hold; an OR, a WITH or
parentheses fail, as an expression this does not read. The PKGBUILD's array is the AND of
its elements, each one quoted string apart from the next, as bash reads them: bash makes
one element of 'a''b'. The Nix package's list is read by nixpkgs' names (NIX_NAMES, each
the spdxId lib/licenses/licenses.nix gives it in the nixpkgs flake.lock pins); a name not
there fails. The four that name the project's licence are read as their files write a
value, bare or in single or double quotes, and to where their files' own readers end it:
YAML's at the end of its line or at a comment after a space (AGPL-3.0-or-later#MIT is one
value to YAML), and a LABEL's at the end of its word, a space, the backslash that continues
the line or the line's end (Docker makes one word of "a"" b" and of a\\ b); a value run on
past that is not read.

  python build/check-package-licenses.py              the tree
  python build/check-package-licenses.py --self-test  the negative controls alone

The tree's check runs the negative controls first, so that a check that has stopped seeing
one of the forms above fails too: each is among them. Among them too is a tree written to a
temporary directory, and to a temporary git repository where there is git (SCAN_TREE):
files of each kind in the places the scan reads, each with a licence field nothing holds,
and files it must not read. The scan must list exactly the first, by its walk and by git's
list alike, and refuse the licence field each holds.

Exit codes: 0 every field names exactly what it must; 1 a field names too little or too
much, is not there or is there twice, a line of those files is a licence field this does
not hold, or a component is undecided; 2 a file could not be read.
"""

import os
import sys

# Nothing is imported by name from this script's own directory, which Python puts first
# on sys.path (build/check-release-chain.py, PATH_GUARD).
sys.path[:] = [p for p in sys.path if os.path.normcase(os.path.realpath(p or os.curdir)) !=
               os.path.normcase(os.path.dirname(os.path.realpath(__file__)))]

import ast
import contextlib
import re
import shutil
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = "hmailserver/source/Server"
PACKAGING = SERVER + "/platform/packaging"
ASSET_SBOM = "build/asset-sbom.py"
CMAKELISTS = SERVER + "/CMakeLists.txt"
SNAPCRAFT = "snap/snapcraft.yaml"
DOCKERFILE = PACKAGING + "/docker/Dockerfile"
CHART = PACKAGING + "/helm/hmailserver/Chart.yaml"
LINUX_IMAGE = "build/ci/linux-image.sh"

OWN = "AGPL-3.0-or-later"
BOOST = "BSL-1.0"

# The components of SOURCE_COMPONENTS compiled into the Linux server, by bom-ref: the
# SPDX identifier the fields name it by (None where SPDX has none), and the RPM fields'
# name of it where that is another (Fedora's, for a notice SPDX does not name).
COMPILED_IN = {
    "quickjs": ("MIT", None),
    # SOURCE_COMPONENTS calls it a "BSD-style notice"; debian/copyright's BSD-1-clause.
    "mime": ("BSD-1-Clause", None),
    "cstdstring": (None, "LicenseRef-Fedora-UltraPermissive"),
    "public-suffix-list": ("MPL-2.0", None),
    "unicode-idna-data": ("Unicode-3.0", None),
    "whatwg-html-named-references": ("BSD-3-Clause", None),
}
# And those it is not, with why.
NOT_COMPILED_IN = {
    "rfc7208-tests": "the SPF conformance tests spf-tests runs, which no package carries",
    "atl-script-host": "compiled into hMailServer.exe only",
    "mail-dkim-corpus": "test data the Windows suite and the fuzzing seeds read",
}

# nixpkgs' names of the licences, each with the spdxId lib/licenses/licenses.nix gives it
# (nixos-26.05.10882.7fc6f2c20af0, which flake.lock pins).
NIX_NAMES = {
    "agpl3Plus": "AGPL-3.0-or-later",
    "mit": "MIT",
    "bsd1": "BSD-1-Clause",
    "mpl20": "MPL-2.0",
    "unicode-30": "Unicode-3.0",
    "bsd3": "BSD-3-Clause",
    "boost": "BSL-1.0",
}

# Each field: what it is, its file, how its value is read, whether it is an RPM field
# (which names CStdString), and whether the file is the CMakeLists itself (whose own
# default decides Boost) or passes the build its options.
FIELDS = (
    ("the project's .rpm (CPACK_RPM_PACKAGE_LICENSE)", CMAKELISTS, "cpack", True),
    ("the RPM spec's License", PACKAGING + "/rpm/hmailserver.spec", "spec", True),
    ("the APKBUILD's license", PACKAGING + "/alpine/APKBUILD", "apkbuild", False),
    ("the PKGBUILD's license", PACKAGING + "/PKGBUILD", "pkgbuild", False),
    ("the Nix package's meta.license", "nix/package.nix", "nix", False),
)

# The fields that name the project's own licence alone: what each is, its file, how its
# value is read, and why it names no other.
OWN_LICENCE_ONLY = (
    ("the snap's license", SNAPCRAFT, "snap",
     "snapd holds a snap's licence to the identifiers of SPDX License List 3.11 (its spdx/licenses.go), "
     "which has no Unicode-3.0, and its snap pack and its install refuse a snap whose licence names another"),
    ("the container image's org.opencontainers.image.licenses", DOCKERFILE, "oci-label",
     "the image is Ubuntu 24.04 with the .deb and what it depends on installed, under the licences the "
     "copyright files in it declare, the .deb's among them"),
    ("the container image's io.artifacthub.package.license", DOCKERFILE, "artifacthub-label",
     "Artifact Hub takes one SPDX identifier there"),
    ("the Helm chart's artifacthub.io/license", CHART, "chart-annotation",
     "Artifact Hub takes one SPDX identifier there, and the chart's files are the project's own"),
)

# A value as the snap's and the chart's YAML writes it - in double or single quotes, or
# bare - to the end of its line or to a comment, which YAML begins only after a space or a
# tab: AGPL-3.0-or-later#MIT is one plain scalar. And as a Dockerfile's LABEL writes it,
# where a value is a word, which ends at a space, at the backslash that continues its line
# or at the line's end: one that runs on past its quotes or past a backslash, which Docker
# makes one word of ("a"" b", a\ b), is not read.
YAML_VALUE = r"""(?:"([^"\n]*)"|'([^'\n]*)'|([^\s"'#][^\n]*?))(?:[ \t]+#[^\n]*)?[ \t]*$"""
LABEL_VALUE = r"""(?:"([^"\n]*)"|'([^'\n]*)'|([^\s"'#\\][^\s#\\]*))(?=[ \t]|\\[ \t]*$|$)"""
# The PKGBUILD's array: each element one quoted string, apart from the next by a space or a
# tab, for bash makes one element of 'a''b' or 'a'"b".
PKGBUILD_ARRAY = re.compile(r"""[ \t]*(?:(?:'[^']*'|"[^"]*")(?:[ \t]+|$))+""")
READERS = {
    "cpack": re.compile(r'^set\(CPACK_RPM_PACKAGE_LICENSE\s+"([^"\n]*)"\s*\)', re.MULTILINE),
    "spec": re.compile(r"^License:[ \t]*(\S[^\n]*?)[ \t]*$", re.MULTILINE),
    "apkbuild": re.compile(r'^license="([^"\n]*)"[ \t]*$', re.MULTILINE),
    "pkgbuild": re.compile(r"^license=\(([^)\n]*)\)[ \t]*$", re.MULTILINE),
    "nix": re.compile(r"^[ \t]*license = with lib\.licenses; \[([^\]\n]*)\];[ \t]*$", re.MULTILINE),
    "snap": re.compile(r"^license:[ \t]*" + YAML_VALUE, re.MULTILINE),
    "oci-label": re.compile(r"^[ \t]*(?:LABEL[ \t]+)?org\.opencontainers\.image\.licenses=" + LABEL_VALUE, re.MULTILINE),
    "artifacthub-label": re.compile(r"^[ \t]*(?:LABEL[ \t]+)?io\.artifacthub\.package\.license=" + LABEL_VALUE,
                                    re.MULTILINE),
    "chart-annotation": re.compile(r"^[ \t]+artifacthub\.io/license:[ \t]*" + YAML_VALUE, re.MULTILINE),
}
BOOST_OPTION = re.compile(r'^option\(HM_BOOST_STATIC "[^"\n]*" (ON|OFF)\)', re.MULTILINE)
BOOST_OFF = "-DHM_BOOST_STATIC=OFF"
IDENTIFIER = re.compile(r"^(?:LicenseRef-)?[A-Za-z0-9][A-Za-z0-9.+-]*$")

# The files the scan reads - the places the recipes of the Linux packages, the snap, the
# container image and the Helm chart are kept, and linux-image.sh, which gives kaniko the
# image's labels that change with each build as --label flags - and the lines of them that
# are a licence field, by the kind of file: a line that is not a comment and that one of
# these takes must be the line a reader of FIELDS or of OWN_LICENCE_ONLY reads in that
# file. (A licence file, such as debian/copyright, is not a field: it is what the fields'
# notices are, and none of these kinds takes it.) The CI files and the other scripts under
# build/ are not read.
SCANNED_DIRECTORIES = ("snap/", "nix/", PACKAGING + "/")
SCANNED_FILES = ("flake.nix", CMAKELISTS, LINUX_IMAGE)
# An image's two licence labels, and AppStream's licence, in each kind of file that can
# carry them.
IMAGE_LICENCE_LABEL = r"\b(?:org\.opencontainers\.image\.licenses|io\.artifacthub\.package\.license)\b"
APPSTREAM_LICENCE = r"<(?:project|metadata)_license\b"
FIELD_LINES = (
    # An RPM spec's License tag, in any case, with or without a space before its colon, as
    # rpm reads a tag.
    (re.compile(r"\.spec(\.in)?$"), re.compile(r"^[ \t]*license[ \t]*:", re.IGNORECASE)),
    # license assigned in an APKBUILD or a PKGBUILD, as the shell that sources it assigns:
    # license=, license+= or an element's license[...]=, anywhere on a line - after && or a
    # ; or a declare - but not a read of it ($license, ${license[@]}) nor another name
    # (_license=, --license=).
    (re.compile(r"(^|/)(APKBUILD|PKGBUILD)$"), re.compile(r"(?<![\w$./-])license(?:\[[^\]\n]*\])?\+?=")),
    # A licence bound in Nix - license =, meta.license =, the name in quotes - or inherited.
    (re.compile(r"\.nix$"), re.compile(r"""\blicen[cs]es?"?[ \t]*=|\binherit\b[^;\n]*\blicen[cs]e\b""")),
    # A CPack variable of a package's licence (CPACK_*PACKAGE_LICENSE*) named by a CMake
    # command: set(), string(), list() and any other that names the variable it changes
    # bare, whatever the case of the command's name; a read is ${...}.
    (re.compile(r"(^|/)(CMakeLists\.txt|[^/]*\.cmake)$"),
     re.compile(r"(?<!\$\{)\bCPACK_[A-Z0-9_]*PACKAGE_LICEN[CS]E[A-Z0-9_]*")),
    # A YAML licence key - license, licence, licenses or licences, prefixed as
    # artifacthub.io/license is, in quotes or not - at the start of a line, after a
    # sequence's dashes, or in a flow mapping or sequence after its {, its [ or a comma.
    (re.compile(r"\.ya?ml(\.in)?$"),
     re.compile(r"""(?:^|[{\[,])[ \t]*(?:-[ \t]+)*["']?(?:[A-Za-z0-9.-]+/)?licen[cs]es?["']?[ \t]*:""")),
    (re.compile(r"(^|/)((Docker|Container)file[^/]*|[^/]*\.(Docker|Container)file|[^/]*\.ya?ml(\.in)?)$"),
     re.compile(IMAGE_LICENCE_LABEL)),
    (re.compile(r"\.xml(\.in)?$"), re.compile(APPSTREAM_LICENCE)),
    # A shell script (*.sh; one without the suffix is not read) sets a label as KEY=VALUE,
    # its key in quotes or not - a builder's --label or --annotation, as linux-image.sh's
    # kaniko is given the others (kaniko 1.24 keeps the Dockerfile's LABEL of a key both
    # name, and adds a key the Dockerfile does not), or a Dockerfile's LABEL it writes from a
    # here-document, which also takes the older LABEL KEY VALUE - and writes AppStream's
    # licence from a here-document. A label it reads back, named in the list of those a test
    # asks for or in the image's JSON, is not a field.
    (re.compile(r"\.sh$"),
     re.compile(IMAGE_LICENCE_LABEL + r"""["']?[ \t]*=|\bLABEL[ \t]+""" + IMAGE_LICENCE_LABEL + "|" + APPSTREAM_LICENCE)),
)


class Unreadable(Exception):
    pass


def read(root, path):
    try:
        with open(os.path.join(root, *path.split("/")), "rb") as f:
            return f.read().decode("utf-8").replace("\r\n", "\n")
    except (OSError, UnicodeDecodeError) as e:
        raise Unreadable("%s cannot be read: %s" % (path, e))


def source_components(text):
    """SOURCE_COMPONENTS of build/asset-sbom.py, as {bom-ref: (name, SPDX or None)}, read
    from the file's syntax tree: the list is literals alone."""
    for node in ast.parse(text).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "SOURCE_COMPONENTS"
                                                for t in node.targets):
            return dict((entry[0], (entry[1], entry[3])) for entry in ast.literal_eval(node.value))
    raise Unreadable("%s has no SOURCE_COMPONENTS list" % ASSET_SBOM)


def spdx_and(expression):
    """The identifiers of an AND of identifiers, or None for any other expression."""
    words = expression.split()
    if not words or len(words) % 2 == 0:
        return None
    names = words[0::2]
    if any(w != "AND" for w in words[1::2]) or not all(IDENTIFIER.match(n) for n in names):
        return None
    if any(n in ("AND", "OR", "WITH") for n in names):
        return None
    return names


def field_value(kind, text):
    """(the identifiers the field names, or None, and what is wrong with it)."""
    found = READERS[kind].findall(text)
    if len(found) != 1:
        return None, "is there %d time(s), not once" % len(found)
    value = found[0]
    if kind == "pkgbuild":
        if not PKGBUILD_ARRAY.fullmatch(value):
            return None, "is not an array of quoted SPDX expressions, each apart from the next: (%s)" % value
        elements = re.findall(r"""'([^']*)'|"([^"]*)\"""", value)
        names = []
        for single, double in elements:
            part = spdx_and(single or double)
            if part is None:
                return None, "holds %r, which is not an AND of SPDX identifiers" % (single or double)
            names.extend(part)
        return names, None
    if kind == "nix":
        names = []
        for name in value.split():
            if name not in NIX_NAMES:
                return None, ("names %s, which NIX_NAMES does not know: add nixpkgs' name with the spdxId its "
                              "lib/licenses/licenses.nix gives it" % name)
            names.append(NIX_NAMES[name])
        return names, None
    names = spdx_and(value)
    if names is None:
        return None, "is %r, which is not an AND of SPDX identifiers" % value
    return names, None


def own_value(kind, text):
    """(the value of a field that names the project's licence alone, or None, and what is wrong)."""
    found = READERS[kind].findall(text)
    if len(found) != 1:
        return None, "is there %d time(s), not once" % len(found)
    return "".join(found[0]), None


def boost_static(kind, text, cmake_default):
    """Whether the build that field is of links Boost statically: the CMake default,
    unless a line of the file that is not a comment passes -DHM_BOOST_STATIC=OFF."""
    if kind == "cpack":
        return cmake_default
    code = [line for line in text.split("\n") if not line.lstrip().startswith("#")]
    return cmake_default and not any(BOOST_OFF in line for line in code)


def in_scope(path):
    return path in SCANNED_FILES or path.startswith(SCANNED_DIRECTORIES)


def field_lines_of(path):
    """The FIELD_LINES patterns that read a file of that name: none for a file of no kind."""
    return [line for name, line in FIELD_LINES if name.search(path)]


def held_columns(texts, own_only):
    """{path: {line number: the column at which what a reader of FIELDS or own_only reads
    ends on the line it begins}}. Each reader begins at the start of a line; one that runs
    on past its line, as CMake's set( does onto its value, has the whole of it."""
    held = {}
    for field in tuple(FIELDS) + tuple(own_only):
        path, kind = field[1], field[2]
        if path in texts:
            text = texts[path]
            for match in READERS[kind].finditer(text):
                start = text.rfind("\n", 0, match.start()) + 1
                held.setdefault(path, {})[text.count("\n", 0, match.start()) + 1] = match.end() - start
    return held


def unheld_fields(texts, own_only):
    """The licence fields in the scanned files among texts that no reader of FIELDS or
    own_only reads, as problems: a line no reader reads that a FIELD_LINES pattern of its
    kind takes, and a line one reads that a pattern takes after where the reader ends - a
    Dockerfile's LABEL takes several key=value pairs on a line, and the image gets the last
    of a key."""
    held = held_columns(texts, own_only)
    problems = []
    for path in sorted(texts):
        patterns = field_lines_of(path) if in_scope(path) else []
        if not patterns:
            continue
        columns = held.get(path, {})
        for number, line in enumerate(texts[path].split("\n"), 1):
            column = columns.get(number)
            if line.lstrip().startswith("#") or not any(p.search(line, column or 0) for p in patterns):
                continue
            if column is None:
                problems.append("%s:%d (%s) is a licence field this does not hold: put it in FIELDS, to name what is "
                                "compiled into the server, or in OWN_LICENCE_ONLY, with why it names the project's "
                                "licence alone" % (path, number, line.strip()))
            else:
                problems.append("%s:%d (%s) has a licence field this does not hold after the field read there: a field "
                                "this holds has its line to itself, and another goes on a line of its own, in FIELDS or "
                                "in OWN_LICENCE_ONLY" % (path, number, line.strip()))
    return problems


def problems_of(texts, own_only=OWN_LICENCE_ONLY):
    """What is wrong, given {path: text} of the CMakeLists, asset-sbom.py, the files of the
    fields and the other files the scan reads."""
    problems = []
    components = source_components(texts[ASSET_SBOM])
    for ref, (name, spdx) in sorted(components.items()):
        if ref in COMPILED_IN:
            ours = COMPILED_IN[ref][0]
            if spdx is not None and spdx != ours:
                problems.append("%s's SOURCE_COMPONENTS gives %s (%s) the licence %s, and COMPILED_IN here %s: say which "
                                "it is in both" % (ASSET_SBOM, ref, name, spdx, ours))
        elif ref not in NOT_COMPILED_IN:
            problems.append("%s's SOURCE_COMPONENTS has %s (%s), which is neither in COMPILED_IN nor in NOT_COMPILED_IN "
                            "in build/check-package-licenses.py: say whether the Linux server compiles it in, and if "
                            "it does, name its licence in every package's licence field" % (ASSET_SBOM, ref, name))
    for ref in sorted(set(COMPILED_IN) | set(NOT_COMPILED_IN)):
        if ref not in components:
            problems.append("build/check-package-licenses.py decides %s, which %s's SOURCE_COMPONENTS no longer has: "
                            "remove it here, or put it back there" % (ref, ASSET_SBOM))
    for what, path, kind, why in own_only:
        value, wrong = own_value(kind, texts[path])
        if wrong:
            problems.append("%s (%s) %s" % (what, path, wrong))
        elif value != OWN:
            problems.append("%s (%s) names %s, and it names the project's licence alone, %s, because %s: name that, or "
                            "move the field to FIELDS and hold it to what is compiled into the server" % (
                                what, path, value or "nothing", OWN, why))
    problems.extend(unheld_fields(texts, own_only))
    default = BOOST_OPTION.findall(texts[CMAKELISTS])
    if len(default) != 1:
        problems.append("%s gives HM_BOOST_STATIC %d default(s), not one: whether a build links Boost statically "
                        "cannot be read" % (CMAKELISTS, len(default)))
        return problems
    cmake_default = default[0] == "ON"
    for what, path, kind, rpm in FIELDS:
        text = texts[path]
        names, wrong = field_value(kind, text)
        if wrong:
            problems.append("%s (%s) %s" % (what, path, wrong))
            continue
        why = {OWN: "the server's own"}
        for ref, (spdx, rpm_name) in COMPILED_IN.items():
            named = rpm_name if (rpm and rpm_name) else spdx
            if named:
                why.setdefault(named, components.get(ref, (ref, None))[0])
        static = boost_static(kind, text, cmake_default)
        if static:
            why.setdefault(BOOST, "Boost, which this build links statically")
        missing = [n for n in why if n not in names]
        if missing:
            problems.append("%s (%s) does not name %s: %s" % (
                what, path, " AND ".join(missing), "; ".join("%s is the licence of %s" % (n, why[n]) for n in missing)))
        extra = [n for n in names if n not in why]
        for name in extra:
            if name == BOOST and not static:
                reason = "this build links the distribution's shared Boost (%s)" % BOOST_OFF
            else:
                reason = "neither the server nor anything compiled into it is under it"
            problems.append("%s (%s) names %s, which it should not: %s" % (what, path, name, reason))
        twice = sorted(set(n for n in names if names.count(n) > 1))
        if twice:
            problems.append("%s (%s) names %s twice" % (what, path, ", ".join(twice)))
    return problems


def scanned_paths(root):
    """The tree's files in SCANNED_DIRECTORIES and SCANNED_FILES of a kind FIELD_LINES reads,
    relative to root: git's list where there is git, else a walk of those places."""
    def wanted(path):
        return in_scope(path) and bool(field_lines_of(path))
    try:
        done = subprocess.run(["git", "-C", root, "ls-files", "-z"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if done.returncode == 0 and done.stdout:
            return sorted(p for p in done.stdout.decode("utf-8", "replace").split("\0") if p and wanted(p))
    except OSError:
        pass
    found = [p for p in SCANNED_FILES if os.path.isfile(os.path.join(root, *p.split("/")))]
    for place in SCANNED_DIRECTORIES:
        for directory, subdirectories, files in os.walk(os.path.join(root, *place.rstrip("/").split("/"))):
            subdirectories[:] = [d for d in subdirectories if d != ".git"]
            for name in files:
                found.append(os.path.relpath(os.path.join(directory, name), root).replace(os.sep, "/"))
    return sorted(set(p for p in found if wanted(p)))


def tree_texts(root):
    paths = ([ASSET_SBOM, CMAKELISTS] + [field[1] for field in FIELDS] + [field[1] for field in OWN_LICENCE_ONLY] +
             scanned_paths(root))
    return dict((path, read(root, path)) for path in sorted(set(paths)))


# ---------------------------------------------------------------- the negative controls

GOOD_SBOM = '''SOURCE_COMPONENTS = [
    ("quickjs", "QuickJS", "q", "MIT", None, "x"),
    ("mime", "MIME", "m", None, "BSD-style notice", "x"),
    ("cstdstring", "CStdString", "s", None, "its own", "x"),
    ("rfc7208-tests", "RFC 7208", "r", None, "BSD-style notice", "x"),
    ("public-suffix-list", "PSL", "p", "MPL-2.0", None, "x"),
    ("unicode-idna-data", "Unicode data", "u", "Unicode-3.0", None, "x"),
    ("whatwg-html-named-references", "HTML references", "w", "BSD-3-Clause", None, "x"),
    ("atl-script-host", "ATL", "a", None, "its own", "x"),
    ("mail-dkim-corpus", "DKIM", "d", "GPL-1.0-or-later OR Artistic-1.0-Perl", None, "x"),
]
'''
GOOD = {
    ASSET_SBOM: GOOD_SBOM,
    CMAKELISTS: ('option(HM_BOOST_STATIC "Link Boost statically" ON)\n'
                 'set(CPACK_RPM_PACKAGE_LICENSE\n   "AGPL-3.0-or-later AND MIT AND BSD-1-Clause AND MPL-2.0 AND '
                 'LicenseRef-Fedora-UltraPermissive AND Unicode-3.0 AND BSD-3-Clause AND BSL-1.0")\n'),
    PACKAGING + "/rpm/hmailserver.spec": ("License:        AGPL-3.0-or-later AND MIT AND BSD-1-Clause AND MPL-2.0 AND "
                                          "LicenseRef-Fedora-UltraPermissive AND Unicode-3.0 AND BSD-3-Clause\n"
                                          "%cmake \\\n  -DHM_BOOST_STATIC=OFF \\\n"),
    PACKAGING + "/alpine/APKBUILD": ('license="AGPL-3.0-or-later AND MIT AND BSD-1-Clause AND MPL-2.0 AND Unicode-3.0 '
                                     'AND BSD-3-Clause"\n\t\t-DHM_BOOST_STATIC=OFF \\\n'),
    PACKAGING + "/PKGBUILD": ("license=('AGPL-3.0-or-later AND MIT AND BSD-1-Clause AND MPL-2.0 AND Unicode-3.0 AND "
                              "BSD-3-Clause AND BSL-1.0')\n"),
    "nix/package.nix": ('    "-DHM_BOOST_STATIC=OFF"\n'
                        "    license = with lib.licenses; [ agpl3Plus mit bsd1 mpl20 unicode-30 bsd3 ];\n"),
    SNAPCRAFT: "name: hmailserver\nlicense: AGPL-3.0-or-later\ngrade: stable\n",
    DOCKERFILE: ('FROM ubuntu:24.04\n'
                 'LABEL org.opencontainers.image.title="hMailServer" \\\n'
                 '      org.opencontainers.image.licenses="AGPL-3.0-or-later" \\\n'
                 '      io.artifacthub.package.license="AGPL-3.0-or-later" \\\n'
                 '      io.artifacthub.package.keywords="mail"\n'),
    CHART: ('apiVersion: v2\nname: hmailserver\nannotations:\n  artifacthub.io/license: AGPL-3.0-or-later\n'
            '  artifacthub.io/prerelease: "false"\n'),
    # The labels kaniko is given, and the image's licence label read back by name.
    LINUX_IMAGE: ('"$executor" \\\n'
                  '    --label "org.opencontainers.image.version=$version" \\\n'
                  '    --label "io.artifacthub.package.prerelease=$prerelease" \\\n'
                  '    --cleanup\n'
                  'for label in org.opencontainers.image.source org.opencontainers.image.licenses; do\n'
                  '  grep -q "\\"$label\\":\\"[^\\"]" "$out/image.json" || fail "the image carries no $label label"\n'
                  'done\n'),
}
# The files of GOOD the scan reads, named here rather than found by the code it tries.
GOOD_SCANNED = (CMAKELISTS, PACKAGING + "/rpm/hmailserver.spec", PACKAGING + "/alpine/APKBUILD", PACKAGING + "/PKGBUILD",
                "nix/package.nix", SNAPCRAFT, DOCKERFILE, CHART, LINUX_IMAGE)
# A tree for the scan's own listing: beside GOOD's files, files of each kind in the places
# the scan reads - one in each place at least, and each optional part of a kind's names
# spelt out by one of them (.spec and .spec.in, .yml and .yaml.in, Dockerfile.<suffix> and
# *.Dockerfile, ...) - each with a licence field nothing holds at its second line. Then a
# file for each reason the scan does not read one: outside those places, of no kind, and,
# written for the walk alone, under a .git directory, and for git alone, not tracked.
SCAN_TREE = {
    "flake.nix": "{\n  meta.license = lib.licenses.mit;\n}\n",
    "nix/tools.nix": "{ lib }: {\n  meta.license = lib.licenses.mit;\n}\n",
    "snap/local/parts.yaml.in": "parts:\n  - license: MIT\n",
    "snap/local/image.sh": "cat > Dockerfile <<'EOF'\nLABEL org.opencontainers.image.licenses MIT\nEOF\n",
    PACKAGING + "/rpm/hmailserver-cli.spec": "Name: hmailserver-cli\nLicense: MIT\n",
    PACKAGING + "/rpm/hmailserver.spec.in": "Name: hmailserver\nlicense: MIT\n",
    PACKAGING + "/alpine/cli/APKBUILD": 'pkgname=hmailserver-cli\nlicense="MIT"\n',
    PACKAGING + "/arch/PKGBUILD": "pkgname=hmailserver-cli\nlicense=('MIT')\n",
    PACKAGING + "/cmake/CMakeLists.txt": 'project(x)\nset(CPACK_RPM_PACKAGE_LICENSE "MIT")\n',
    PACKAGING + "/cpack-extra.cmake": 'include(x)\nset(CPACK_FREEBSD_PACKAGE_LICENSE "MIT")\n',
    PACKAGING + "/helm/other/Chart.yaml": "apiVersion: v2\n'license': MIT\n",
    PACKAGING + "/docker/artifacthub-repo.yml": "repositoryID: x\nlicence: MIT\n",
    PACKAGING + "/docker/compose.yaml.in": "services:\n  x: {build: {labels: {org.opencontainers.image.licenses: MIT}}}\n",
    PACKAGING + "/docker/compose.yml": ("# a Compose file's labels as a list\n"
                                        "services: {x: {labels: [io.artifacthub.package.license=MIT]}}\n"),
    PACKAGING + "/docker/Dockerfile.alpine": 'FROM alpine\nLABEL org.opencontainers.image.licenses="MIT"\n',
    PACKAGING + "/docker/Containerfile": "FROM alpine\nLABEL io.artifacthub.package.license=MIT\n",
    PACKAGING + "/docker/hmailserver.Dockerfile": "FROM alpine\nLABEL org.opencontainers.image.licenses=MIT\n",
    PACKAGING + "/docker/hmailserver.Containerfile": 'FROM alpine\n  io.artifacthub.package.license="MIT"\n',
    PACKAGING + "/docker/build.sh": "set -e\nbuildah config --label io.artifacthub.package.license=MIT x\n",
    PACKAGING + "/appimage/hmailserver.metainfo.xml": "<component>\n  <project_license>MIT</project_license>\n</component>\n",
    PACKAGING + "/appimage/hmailserver.appdata.xml.in": ("<component>\n  <metadata_license>FSFAP</metadata_license>\n"
                                                         "</component>\n"),
    PACKAGING + "/appimage/metainfo.sh": ("cat > x.xml <<'XML'\n<component><project_license>MIT</project_license></component>\n"
                                          "XML\n"),
}
SCAN_TREE_NOT_READ = {
    "build/ci/linux.Dockerfile": "FROM x\nLABEL org.opencontainers.image.licenses=MIT\n",
    PACKAGING + "/README.md": "# x\nlicense: MIT\n",
}
SCAN_TREE_UNDER_GIT_DIRECTORY = {"nix/.git/config.nix": "{\n  license = 1;\n}\n"}
SCAN_TREE_UNTRACKED = {"snap/local/untracked.yaml": "name: x\nlicense: MIT\n"}


def write_tree(root, files):
    for path, text in files.items():
        full = os.path.join(root, *path.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)


def remove_tree(root):
    # git writes its objects read-only, which Windows will not delete as they are.
    for directory, _, names in os.walk(root):
        for name in names:
            try:
                os.chmod(os.path.join(directory, name), 0o600)
            except OSError:
                pass
    shutil.rmtree(root, ignore_errors=True)


@contextlib.contextmanager
def environment_without(names):
    """os.environ without those variables while it is entered."""
    saved = dict((name, os.environ.pop(name)) for name in names if name in os.environ)
    try:
        yield
    finally:
        os.environ.update(saved)


def scan_tree_failures():
    """What is wrong with the scan's listing and reading of SCAN_TREE: by the walk, over a
    temporary directory, and by git's list, over a temporary git repository, which is the
    way the checkers job takes. Returns (the failures, what could not be tried)."""
    failures = []
    skipped = []
    listed = sorted(GOOD_SCANNED + tuple(SCAN_TREE))
    refused = sorted("%s:2" % path for path in SCAN_TREE)

    def differences(expected, got):
        """What got leaves out of expected and has besides, or how it lists the same."""
        if sorted(set(expected)) != sorted(set(got)):
            return "it left out %s and had %s besides" % (sorted(set(expected) - set(got)) or "nothing",
                                                          sorted(set(got) - set(expected)) or "nothing")
        return "it had them as %s, not as %s" % (got, expected)

    def tried(how, root):
        found = scanned_paths(root)
        if found != listed:
            failures.append("the scan's listing of a tree by %s is not the files it reads there: %s" % (
                how, differences(listed, found)))
        try:
            problems = problems_of(tree_texts(root))
        except Unreadable as e:
            problems = [str(e)]
        unheld = re.compile(r"^(\S+:\d+) \(.* is a licence field this does not hold")
        where = sorted(m.group(1) for m in (unheld.match(p) for p in problems) if m)
        others = [p for p in problems if not unheld.match(p)]
        if where != refused:
            failures.append("the scan's reading of a tree by %s did not refuse each licence field nothing holds there once: "
                            "%s" % (how, differences(refused, where)))
        if others:
            failures.append("the scan's reading of a tree by %s found %s" % (how, others))

    # Without the variables a git hook or a caller sets to point git at a repository of its
    # own (git rev-parse --local-env-vars), with one of which git would list that
    # repository, not these.
    local = []
    git = shutil.which("git")
    if git:
        done = subprocess.run([git, "rev-parse", "--local-env-vars"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if done.returncode != 0:
            failures.append("git rev-parse --local-env-vars failed: %s" % done.stderr.decode("utf-8", "replace").strip())
            return failures, skipped
        local = done.stdout.decode("utf-8", "replace").split()
    with environment_without(local):
        walked = tempfile.mkdtemp(prefix="check-package-licenses-walk-")
        try:
            write_tree(walked, dict(GOOD, **dict(SCAN_TREE, **dict(SCAN_TREE_NOT_READ, **SCAN_TREE_UNDER_GIT_DIRECTORY))))
            tried("the walk", walked)
        finally:
            remove_tree(walked)
        if not git:
            skipped.append("the scan's listing by git's list, for there is no git here")
            return failures, skipped
        repository = tempfile.mkdtemp(prefix="check-package-licenses-git-")
        try:
            tracked = dict(GOOD, **dict(SCAN_TREE, **SCAN_TREE_NOT_READ))
            write_tree(repository, tracked)
            # The files go into the index as they are, whatever the caller's own git says of
            # line ends or of what it ignores.
            for what, arguments in (("init", ["init", "-q"]),
                                    ("add", ["-c", "core.autocrlf=false", "-c", "core.safecrlf=false", "add", "-f", "--"] +
                                     sorted(tracked))):
                done = subprocess.run([git, "-C", repository] + arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                if done.returncode != 0:
                    failures.append("git %s in a temporary repository failed: %s" % (
                        what, done.stderr.decode("utf-8", "replace").strip()))
                    return failures, skipped
            write_tree(repository, SCAN_TREE_UNTRACKED)
            tried("git's list", repository)
        finally:
            remove_tree(repository)
    return failures, skipped


def self_test():
    failures = []

    def expect(what, texts, wanted, own_only=OWN_LICENCE_ONLY):
        try:
            found = problems_of(texts, own_only)
        except Unreadable as e:
            found = [str(e)]
        if wanted is None:
            if found:
                failures.append("%s: expected no problem, got %s" % (what, found))
            return
        for each in ([wanted] if isinstance(wanted, str) else wanted):
            if not any(each in p for p in found):
                failures.append("%s: expected a problem saying %r, got %s" % (what, each, found))

    def changed(path, old, new):
        texts = dict(GOOD)
        if old not in texts[path]:
            failures.append("the fixture of %s has no %r to change" % (path, old))
        texts[path] = texts[path].replace(old, new)
        return texts

    def added(path, text):
        texts = dict(GOOD)
        texts[path] = text
        return texts

    expect("the good fixture", GOOD, None)
    spec = PACKAGING + "/rpm/hmailserver.spec"
    apk = PACKAGING + "/alpine/APKBUILD"
    pkgbuild = PACKAGING + "/PKGBUILD"
    nix = "nix/package.nix"
    # 6.3.6's and batch48's fields, short of the two data sets' licences.
    expect("the .rpm's tag without the data sets", changed(CMAKELISTS, " AND Unicode-3.0 AND BSD-3-Clause", ""),
           "CPACK_RPM_PACKAGE_LICENSE) (%s) does not name Unicode-3.0 AND BSD-3-Clause" % CMAKELISTS)
    expect("the spec without Unicode's", changed(spec, " AND Unicode-3.0", ""), "does not name Unicode-3.0")
    expect("the APKBUILD without the WHATWG's", changed(apk, " AND BSD-3-Clause", ""), "does not name BSD-3-Clause")
    expect("the PKGBUILD with the AGPL alone", changed(pkgbuild, " AND MIT AND BSD-1-Clause AND MPL-2.0 AND Unicode-3.0 AND "
                                                       "BSD-3-Clause AND BSL-1.0", ""),
           "does not name MIT AND BSD-1-Clause AND MPL-2.0 AND Unicode-3.0 AND BSD-3-Clause AND BSL-1.0")
    expect("the Nix package without the data sets", changed(nix, " unicode-30 bsd3", ""),
           "does not name Unicode-3.0 AND BSD-3-Clause")
    # Too much, and in the wrong form.
    expect("an APKBUILD naming a licence nothing is under", changed(apk, " AND BSD-3-Clause\"", " AND BSD-3-Clause AND GPL-2.0-only\""),
           "names GPL-2.0-only, which it should not")
    expect("a shared-Boost build naming Boost", changed(spec, "BSD-3-Clause\n", "BSD-3-Clause AND BSL-1.0\n"),
           "this build links the distribution's shared Boost")
    expect("the PKGBUILD naming Fedora's CStdString name", changed(pkgbuild, "AND BSL-1.0'", "AND BSL-1.0 AND LicenseRef-Fedora-UltraPermissive'"),
           "names LicenseRef-Fedora-UltraPermissive, which it should not")
    expect("an RPM field without CStdString's", changed(spec, " AND LicenseRef-Fedora-UltraPermissive", ""),
           "does not name LicenseRef-Fedora-UltraPermissive")
    expect("an OR", changed(apk, "AND BSD-3-Clause\"", "OR BSD-3-Clause\""), "not an AND of SPDX identifiers")
    expect("parentheses", changed(spec, "License:        AGPL", "License:        (AGPL"), "not an AND of SPDX identifiers")
    expect("a name nixpkgs does not have", changed(nix, "unicode-30", "unicode30"), "names unicode30, which NIX_NAMES does not know")
    expect("a field named twice", changed(apk, 'license="AGPL', 'license="MIT"\nlicense="AGPL'), "is there 2 time(s), not once")
    expect("a field gone", changed(nix, "license = with", "licence = with"), "is there 0 time(s), not once")
    expect("an identifier twice", changed(pkgbuild, "('AGPL-3.0-or-later AND", "('MIT' 'AGPL-3.0-or-later AND"), "names MIT twice")
    expect("a PKGBUILD element that is not quoted", changed(pkgbuild, "license=('AGPL", "license=(MIT 'AGPL"),
           "is not an array of quoted SPDX expressions")
    # A static build is held to Boost's licence; one made shared is not.
    expect("a static build without Boost's", changed(pkgbuild, " AND BSL-1.0", ""), "does not name BSL-1.0")
    expect("the PKGBUILD made a shared-Boost build",
           dict(changed(pkgbuild, " AND BSL-1.0", ""), **{pkgbuild: GOOD[pkgbuild].replace(" AND BSL-1.0", "") +
                                                          "      -DHM_BOOST_STATIC=OFF \\\n"}), None)
    expect("the option named in a comment alone", changed(pkgbuild, "BSL-1.0')\n", "BSL-1.0')\n# unless -DHM_BOOST_STATIC=OFF\n"),
           None)
    expect("no HM_BOOST_STATIC default",changed(CMAKELISTS, 'option(HM_BOOST_STATIC "Link Boost statically" ON)\n', ""),
           "gives HM_BOOST_STATIC 0 default(s), not one")
    # The components, decided against SOURCE_COMPONENTS.
    expect("a new component not decided", changed(ASSET_SBOM, "]\n", '    ("zstd", "zstd", "z", "BSD-3-Clause", None, "x"),\n]\n'),
           "has zstd (zstd), which is neither in COMPILED_IN nor in NOT_COMPILED_IN")
    expect("a decided component gone", changed(ASSET_SBOM, '    ("mime", "MIME", "m", None, "BSD-style notice", "x"),\n', ""),
           "decides mime, which build/asset-sbom.py's SOURCE_COMPONENTS no longer has")
    expect("a component whose licence changed", changed(ASSET_SBOM, '"q", "MIT"', '"q", "Apache-2.0"'),
           "gives quickjs (QuickJS) the licence Apache-2.0, and COMPILED_IN here MIT")
    expect("no SOURCE_COMPONENTS", changed(ASSET_SBOM, "SOURCE_COMPONENTS = [", "COMPONENTS = ["), "has no SOURCE_COMPONENTS list")
    # The four that name the project's licence alone, each held to it.
    expect("the snap naming what is compiled in as well", changed(SNAPCRAFT, "license: AGPL-3.0-or-later",
                                                                  "license: AGPL-3.0-or-later AND MIT"),
           "the snap's license (%s) names AGPL-3.0-or-later AND MIT, and it names the project's licence alone" % SNAPCRAFT)
    expect("the image's OCI label naming what is compiled in as well",
           changed(DOCKERFILE, 'licenses="AGPL-3.0-or-later"', 'licenses="AGPL-3.0-or-later AND MIT AND BSD-1-Clause AND '
                                                              'MPL-2.0 AND Unicode-3.0 AND BSD-3-Clause AND BSL-1.0"'),
           "the container image's org.opencontainers.image.licenses (%s) names AGPL-3.0-or-later AND MIT" % DOCKERFILE)
    expect("the image's Artifact Hub label naming another", changed(DOCKERFILE, 'package.license="AGPL-3.0-or-later"',
                                                                    "package.license='MIT'"),
           "the container image's io.artifacthub.package.license (%s) names MIT" % DOCKERFILE)
    expect("the chart's annotation naming another", changed(CHART, "license: AGPL-3.0-or-later", "license: Apache-2.0"),
           "the Helm chart's artifacthub.io/license (%s) names Apache-2.0" % CHART)
    expect("the chart's annotation empty", changed(CHART, "license: AGPL-3.0-or-later", 'license: ""'),
           "artifacthub.io/license (%s) names nothing" % CHART)
    expect("the chart's annotation in quotes", changed(CHART, "license: AGPL-3.0-or-later", 'license: "AGPL-3.0-or-later"'),
           None)
    expect("the snap's licence after a comment of its own", changed(SNAPCRAFT, "license: AGPL-3.0-or-later",
                                                                    "license: AGPL-3.0-or-later  # the project's"), None)
    expect("the snap's licence gone", changed(SNAPCRAFT, "license: AGPL", "# license: AGPL"),
           "the snap's license (%s) is there 0 time(s), not once" % SNAPCRAFT)
    expect("an image label twice", changed(DOCKERFILE, "      io.artifacthub.package.license",
                                           '      org.opencontainers.image.licenses="AGPL-3.0-or-later" \\\n'
                                           "      io.artifacthub.package.license"),
           "org.opencontainers.image.licenses (%s) is there 2 time(s), not once" % DOCKERFILE)
    # And no licence field that nothing holds in the files the scan reads.
    expect("a licence label in another Dockerfile",
           added(PACKAGING + "/docker/Dockerfile.alpine", 'FROM alpine\nLABEL org.opencontainers.image.licenses="MIT"\n'),
           "%s/docker/Dockerfile.alpine:2 (LABEL org.opencontainers.image.licenses=\"MIT\") is a licence field this "
           "does not hold" % PACKAGING)
    expect("a licence label in a Containerfile named for its base",
           added(PACKAGING + "/docker/alpine.Containerfile", 'FROM alpine\nLABEL io.artifacthub.package.license=MIT\n'),
           "%s/docker/alpine.Containerfile:2 (LABEL io.artifacthub.package.license=MIT) is a licence field this "
           "does not hold" % PACKAGING)
    expect("a licence key of the chart's own", changed(CHART, "annotations:\n", "license: MIT\nannotations:\n"),
           "%s:3 (license: MIT) is a licence field this does not hold" % CHART)
    expect("a CPack component's licence", changed(CMAKELISTS, "set(CPACK_RPM_PACKAGE_LICENSE\n",
                                                  'set(CPACK_RPM_CLI_PACKAGE_LICENSE "MIT")\nset(CPACK_RPM_PACKAGE_LICENSE\n'),
           "%s:2 (set(CPACK_RPM_CLI_PACKAGE_LICENSE \"MIT\")) is a licence field this does not hold" % CMAKELISTS)
    expect("a Nix module's licence", added("nix/module.nix", "{ lib }:\n{\n  meta.license = lib.licenses.mit;\n}\n"),
           "nix/module.nix:3 (meta.license = lib.licenses.mit;) is a licence field this does not hold")
    expect("an AppStream licence", added(PACKAGING + "/hmailserver.metainfo.xml",
                                         "<component>\n  <project_license>AGPL-3.0-or-later</project_license>\n</component>\n"),
           "%s/hmailserver.metainfo.xml:2 (<project_license>AGPL-3.0-or-later</project_license>) is a licence field "
           "this does not hold" % PACKAGING)
    expect("a snap's licence key in a part", changed(SNAPCRAFT, "grade: stable\n", "grade: stable\nparts:\n  x:\n    license: MIT\n"),
           "%s:6 (license: MIT) is a licence field this does not hold" % SNAPCRAFT)
    expect("a licence label named in a comment alone",
           changed(DOCKERFILE, "LABEL ", '# org.opencontainers.image.licenses="MIT" is the label below\nLABEL '), None)
    expect("a licence label in a file outside them", added("build/ci/linux.Dockerfile",
                                                           'LABEL org.opencontainers.image.licenses="MIT"\n'), None)
    expect("a licence file, not a field", added(PACKAGING + "/debian-src/debian/copyright", "License: MIT\n"), None)
    # A field nothing holds beside one a reader holds, and in another file of the kind: the
    # self-test passed a scan that had stopped reading a Compose file's labels, a PKGBUILD
    # or an APKBUILD, a spec or a YAML list's items, until these (release review 6.3.7, the
    # re-check of RB-2).
    expect("a licence label of a Compose file's build",
           added(PACKAGING + "/docker/docker-compose.yml",
                 'services:\n  hmailserver:\n    build:\n      labels:\n        org.opencontainers.image.licenses: "MIT"\n'),
           '%s/docker/docker-compose.yml:5 (org.opencontainers.image.licenses: "MIT") is a licence field this does not '
           "hold" % PACKAGING)
    expect("a split package's licence in the PKGBUILD",
           changed(pkgbuild, "BSL-1.0')\n", "BSL-1.0')\npackage_hmailserver-cli() {\n  license=('MIT')\n}\n"),
           "%s:3 (license=('MIT')) is a licence field this does not hold" % pkgbuild)
    expect("a licence added to the PKGBUILD's", changed(pkgbuild, "BSL-1.0')\n", "BSL-1.0')\nlicense+=('MIT')\n"),
           "%s:2 (license+=('MIT')) is a licence field this does not hold" % pkgbuild)
    expect("a subpackage's licence in the APKBUILD",
           changed(apk, "-DHM_BOOST_STATIC=OFF \\\n", "-DHM_BOOST_STATIC=OFF \\\ncli() {\n\tlicense=\"MIT\"\n}\n"),
           '%s:4 (license="MIT") is a licence field this does not hold' % apk)
    expect("a second spec", added(PACKAGING + "/rpm/hmailserver-cli.spec", "Name: hmailserver-cli\nLicense: MIT\n"),
           "%s/rpm/hmailserver-cli.spec:2 (License: MIT) is a licence field this does not hold" % PACKAGING)
    expect("a licence key of a YAML list", changed(CHART, "annotations:\n", "maintainers:\n  - license: MIT\nannotations:\n"),
           "%s:4 (- license: MIT) is a licence field this does not hold" % CHART)
    # A shell script's fields: a label kaniko is given, and AppStream's licence written from
    # a here-document; but not a label read back (GOOD's linux-image.sh) or named in a
    # comment, nor one in a script outside them. The scan read no shell script until these
    # (the re-check of RB-2).
    expect("a licence label kaniko is given in linux-image.sh",
           changed(LINUX_IMAGE, "    --cleanup\n", '    --label "org.opencontainers.image.licenses=MIT" \\\n    --cleanup\n'),
           '%s:4 (--label "org.opencontainers.image.licenses=MIT" \\) is a licence field this does not hold' % LINUX_IMAGE)
    expect("AppStream's licence written by build-appimage.sh",
           added(PACKAGING + "/build-appimage.sh", "cat > \"${APPDIR}/usr/share/metainfo/hmailserver.appdata.xml\" <<'XML'\n"
                 "<component><project_license>MIT</project_license></component>\nXML\n"),
           "%s/build-appimage.sh:2 (<component><project_license>MIT</project_license></component>) is a licence field "
           "this does not hold" % PACKAGING)
    expect("a licence label kaniko is given, in a comment alone",
           changed(LINUX_IMAGE, "    --cleanup\n", '    --cleanup\n# --label "org.opencontainers.image.licenses=MIT" would win\n'),
           None)
    expect("a licence label given in a script outside them",
           added("build/ci/container-image-push.sh", 'regctl image mod --label "org.opencontainers.image.licenses=MIT" x\n'),
           None)
    # The line of a field this holds carries that field alone, and the value is read as the
    # file's own reader reads it: a LABEL's to the end of its word, a YAML value to a comment
    # after a space, and a PKGBUILD array's elements each apart from the next. The scan
    # skipped a line a reader held whole and the readers stopped at the value, so that
    # another licence label on that line, which the image gets, a value run on past its
    # quotes and an element of two quoted strings passed, and the check printed the value it
    # had read (the check of RB-2's third round).
    expect("another licence label after the one held on its line",
           changed(DOCKERFILE, 'package.license="AGPL-3.0-or-later" \\\n',
                   'package.license="AGPL-3.0-or-later" org.opencontainers.image.licenses="MIT" \\\n'),
           '%s:4 (io.artifacthub.package.license="AGPL-3.0-or-later" org.opencontainers.image.licenses="MIT" \\) has a '
           "licence field this does not hold after the field read there" % DOCKERFILE)
    expect("the held licence label again on its line",
           changed(DOCKERFILE, 'package.license="AGPL-3.0-or-later" \\\n',
                   'package.license="AGPL-3.0-or-later" io.artifacthub.package.license="MIT" \\\n'),
           '%s:4 (io.artifacthub.package.license="AGPL-3.0-or-later" io.artifacthub.package.license="MIT" \\) has a '
           "licence field this does not hold after the field read there" % DOCKERFILE)
    expect("a licence label with its key quoted after the one held on its line",
           changed(DOCKERFILE, 'package.license="AGPL-3.0-or-later" \\\n',
                   'package.license="AGPL-3.0-or-later" "org.opencontainers.image.licenses"="MIT" \\\n'),
           '%s:4 (io.artifacthub.package.license="AGPL-3.0-or-later" "org.opencontainers.image.licenses"="MIT" \\) has a '
           "licence field this does not hold after the field read there" % DOCKERFILE)
    expect("a licence label's value run on past its quotes",
           changed(DOCKERFILE, 'licenses="AGPL-3.0-or-later" \\', 'licenses="AGPL-3.0-or-later"" AND MIT" \\'),
           ["the container image's org.opencontainers.image.licenses (%s) is there 0 time(s), not once" % DOCKERFILE,
            '%s:3 (org.opencontainers.image.licenses="AGPL-3.0-or-later"" AND MIT" \\) is a licence field this does not '
            "hold" % DOCKERFILE])
    expect("a licence label's bare value run on past a backslash",
           changed(DOCKERFILE, 'package.license="AGPL-3.0-or-later" \\', "package.license=AGPL-3.0-or-later\\ AND\\ MIT \\"),
           "the container image's io.artifacthub.package.license (%s) is there 0 time(s), not once" % DOCKERFILE)
    expect("a licence label's bare value", changed(DOCKERFILE, 'package.license="AGPL-3.0-or-later"',
                                                   "package.license=AGPL-3.0-or-later"), None)
    expect("a licence label's value straight before its line's continuation",
           changed(DOCKERFILE, 'licenses="AGPL-3.0-or-later" \\', 'licenses="AGPL-3.0-or-later"\\'), None)
    expect("a licence label last in its LABEL",
           changed(DOCKERFILE, '      io.artifacthub.package.license="AGPL-3.0-or-later" \\\n'
                               '      io.artifacthub.package.keywords="mail"\n',
                   '      io.artifacthub.package.keywords="mail" \\\n'
                   '      io.artifacthub.package.license="AGPL-3.0-or-later"\n'), None)
    expect("the chart's licence with a # straight after it",
           changed(CHART, "license: AGPL-3.0-or-later", "license: AGPL-3.0-or-later#MIT"),
           "the Helm chart's artifacthub.io/license (%s) names AGPL-3.0-or-later#MIT" % CHART)
    expect("a PKGBUILD element of two quoted strings", changed(pkgbuild, "BSD-1-Clause AND MPL-2.0", "BSD-1-Clause'' MPL-2.0"),
           "is not an array of quoted SPDX expressions, each apart from the next")
    # Each form of a licence field that a kind's own tool takes. The scan read a CMake file's
    # set() alone, an APKBUILD's or a PKGBUILD's license= and license+= at the start of a
    # line alone, a YAML key at the start of a line or after one dash alone, a Nix licence
    # bound with its name bare alone and a shell script's label with its key bare alone, so
    # that a string(APPEND) or a string(REPLACE) of the .rpm's tag, which CPack takes, an
    # element of the PKGBUILD's array and a flow mapping's licence passed; and no case tried
    # a CMake command in capitals or a spec's License with a space before its colon (the
    # check of RB-2's third round).
    cpack_end = 'BSD-3-Clause AND BSL-1.0")\n'
    expect("the .rpm's tag added to by string()",
           changed(CMAKELISTS, cpack_end, cpack_end + 'string(APPEND CPACK_RPM_PACKAGE_LICENSE " AND GPL-2.0-only")\n'),
           '%s:4 (string(APPEND CPACK_RPM_PACKAGE_LICENSE " AND GPL-2.0-only")) is a licence field this does not hold'
           % CMAKELISTS)
    expect("the .rpm's tag cut by string(REPLACE)",
           changed(CMAKELISTS, cpack_end, cpack_end + 'string(REPLACE " AND Unicode-3.0 AND BSD-3-Clause" "" '
                                                      'CPACK_RPM_PACKAGE_LICENSE "${CPACK_RPM_PACKAGE_LICENSE}")\n'),
           '%s:4 (string(REPLACE " AND Unicode-3.0 AND BSD-3-Clause" "" CPACK_RPM_PACKAGE_LICENSE '
           '"${CPACK_RPM_PACKAGE_LICENSE}")) is a licence field this does not hold' % CMAKELISTS)
    expect("a CPack licence added to by list()",
           changed(CMAKELISTS, cpack_end, cpack_end + 'list(APPEND CPACK_FREEBSD_PACKAGE_LICENSE "MIT")\n'),
           '%s:4 (list(APPEND CPACK_FREEBSD_PACKAGE_LICENSE "MIT")) is a licence field this does not hold' % CMAKELISTS)
    expect("a CPack licence set in capitals", changed(CMAKELISTS, cpack_end, cpack_end + 'SET(CPACK_FREEBSD_PACKAGE_LICENSE "MIT")\n'),
           '%s:4 (SET(CPACK_FREEBSD_PACKAGE_LICENSE "MIT")) is a licence field this does not hold' % CMAKELISTS)
    expect("a CPack licence of a longer name",
           changed(CMAKELISTS, cpack_end, cpack_end + 'set(CPACK_NUGET_PACKAGE_LICENSE_EXPRESSION "MIT")\n'),
           '%s:4 (set(CPACK_NUGET_PACKAGE_LICENSE_EXPRESSION "MIT")) is a licence field this does not hold' % CMAKELISTS)
    expect("the .rpm's tag read", changed(CMAKELISTS, cpack_end, cpack_end + 'message(STATUS "The tag: ${CPACK_RPM_PACKAGE_LICENSE}")\n'),
           None)
    expect("an element of the PKGBUILD's licence", changed(pkgbuild, "BSL-1.0')\n", "BSL-1.0')\nlicense[1]='GPL-2.0-only'\n"),
           "%s:2 (license[1]='GPL-2.0-only') is a licence field this does not hold" % pkgbuild)
    expect("a licence added to the PKGBUILD's after &&, and one declared",
           changed(pkgbuild, "BSL-1.0')\n", "BSL-1.0')\n[[ $CARCH == aarch64 ]] && license+=('GPL-2.0-only')\n"
                                            "declare -ga license=('MIT')\n"),
           ["%s:2 ([[ $CARCH == aarch64 ]] && license+=('GPL-2.0-only')) is a licence field this does not hold" % pkgbuild,
            "%s:3 (declare -ga license=('MIT')) is a licence field this does not hold" % pkgbuild])
    expect("the PKGBUILD's licence read", changed(pkgbuild, "BSL-1.0')\n", "BSL-1.0')\nprintf '%s\\n' \"${license[@]}\" \"$license\"\n"),
           None)
    expect("a licence key in a YAML flow mapping",
           added(PACKAGING + "/helm/hmailserver-cli/Chart.yaml", "apiVersion: v2\nannotations: {artifacthub.io/license: MIT}\n"),
           "%s/helm/hmailserver-cli/Chart.yaml:2 (annotations: {artifacthub.io/license: MIT}) is a licence field this does not "
           "hold" % PACKAGING)
    expect("licence keys after a flow's comma, in a flow sequence, in a sequence of sequences, spelt licenses and in "
           "double quotes",
           added(PACKAGING + "/helm/hmailserver/ci/extra-values.yaml",
                 "image: {repository: x, license: MIT}\nkeywords: [license: MIT]\nitems:\n  - - license: MIT\nlicenses: [MIT]\n"
                 'quoted: {"licence": MIT}\n'),
           ["%s/helm/hmailserver/ci/extra-values.yaml:%s is a licence field this does not hold" % (PACKAGING, where)
            for where in ("1 (image: {repository: x, license: MIT})", "2 (keywords: [license: MIT])", "4 (- - license: MIT)",
                          "5 (licenses: [MIT])", '6 (quoted: {"licence": MIT})')])
    expect("a second spec's License with a space before its colon",
           added(PACKAGING + "/rpm/hmailserver-cli.spec", "Name: hmailserver-cli\nLicense : MIT\n"),
           "%s/rpm/hmailserver-cli.spec:2 (License : MIT) is a licence field this does not hold" % PACKAGING)
    expect("a Nix licence inherited",
           added("nix/cli.nix", "{ hmailserver, quickjs }:\nhmailserver.overrideAttrs (old: {\n"
                                "  meta = old.meta // { inherit (quickjs.meta) license; };\n})\n"),
           "nix/cli.nix:3 (meta = old.meta // { inherit (quickjs.meta) license; };) is a licence field this does not hold")
    expect("a Nix licence's name in quotes",
           added("nix/cli.nix", "{ hmailserver, quickjs }:\nhmailserver.overrideAttrs (old: {\n"
                                "  meta = old.meta // { \"license\" = quickjs.meta.license; };\n})\n"),
           'nix/cli.nix:3 (meta = old.meta // { "license" = quickjs.meta.license; };) is a licence field this does not hold')
    expect("nixpkgs' licences inherited", added("nix/licences.nix", "{ lib }:\nlet inherit (lib.licenses) mit; in [ mit ]\n"),
           None)
    expect("a licence label with its key in quotes, written by a script",
           added(PACKAGING + "/docker/make-dockerfile.sh", "cat > Dockerfile <<'EOF'\nLABEL org.opencontainers.image.title=\"x\" \\\n"
                 "      \"io.artifacthub.package.license\"=\"MIT\"\nEOF\n"),
           '%s/docker/make-dockerfile.sh:3 ("io.artifacthub.package.license"="MIT") is a licence field this does not hold'
           % PACKAGING)
    # As first written the check held the five alone, and said they were every licence
    # field of the Linux packages: the other four are licence fields, and nothing held them.
    expect("the five alone", GOOD,
           ["%s:2 (license: AGPL-3.0-or-later) is a licence field this does not hold" % SNAPCRAFT,
            "%s:3 (org.opencontainers.image.licenses=\"AGPL-3.0-or-later\" \\) is a licence field this does not hold"
            % DOCKERFILE,
            "%s:4 (io.artifacthub.package.license=\"AGPL-3.0-or-later\" \\) is a licence field this does not hold"
            % DOCKERFILE,
            "%s:4 (artifacthub.io/license: AGPL-3.0-or-later) is a licence field this does not hold" % CHART], ())
    # The scan's own listing of a tree, by its walk and by git's list.
    found, skipped = scan_tree_failures()
    failures.extend(found)
    return failures, skipped


def main(argv):
    if argv not in ([], ["--self-test"]):
        print("usage: check-package-licenses.py [--self-test]")
        return 2
    failures, skipped = self_test()
    if failures:
        print("FAILED: the negative controls - the check no longer sees what it reads:")
        for failure in failures:
            print("  " + failure)
        return 1
    if argv == ["--self-test"]:
        print("OK: the negative controls, each refused as it should be")
        for each in skipped:
            print("  not tried: %s" % each)
        return 0
    for each in skipped:
        print("the negative controls passed; not tried: %s" % each)
    try:
        texts = tree_texts(ROOT)
        problems = problems_of(texts)
    except Unreadable as e:
        print("FAILED: %s" % e)
        return 2
    if problems:
        print("FAILED: the Linux packages' licence fields and what is compiled into the server do not agree:")
        for problem in problems:
            print("  " + problem)
        return 1
    print("OK: the five licence fields each name the server's licence and those of what is compiled into it:")
    for what, path, kind, _ in FIELDS:
        print("  %s: %s" % (what, " AND ".join(field_value(kind, texts[path])[0])))
    print("the four that name the project's licence alone do:")
    for what, path, kind, _ in OWN_LICENCE_ONLY:
        print("  %s: %s" % (what, own_value(kind, texts[path])[0]))
    scanned = [p for p in texts if in_scope(p) and field_lines_of(p)]
    print("and no other line of the %d files the scan reads - those of %s, by the kind of file - is a licence field, nor "
          "is anything after the field on the line of each" % (len(scanned), ", ".join(SCANNED_DIRECTORIES + SCANNED_FILES)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
