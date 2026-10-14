#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every licence field of the Linux packages names the licences of what is compiled into the server.

Five files say, in a field a package manager, a distribution and an auditor read, under
which licences a Linux package of the server goes out: the project's own .rpm
(CPACK_RPM_PACKAGE_LICENSE in CMakeLists.txt), the RPM spec's License, the APKBUILD's
license=, the PKGBUILD's license= and the Nix package's meta.license. Each must name the
server's own licence and the licence of every third-party component compiled into the
server that an identifier names, and nothing else: hmailserver/docs/PackagesRpm.md, "The
licences", says so of the tag. Until 6.3.7 four of them named neither licence of the two
data sets idn-uts46 compiled in - Unicode's data for UTS 46 (Unicode-3.0) and HTML's list
of named character references (BSD-3-Clause) - and the PKGBUILD named the AGPL alone
(release review 6.3.7, B-DOC-1).

What is compiled in is decided component by component against build/asset-sbom.py's
SOURCE_COMPONENTS, the third-party parts of the source tarball as
hmailserver/docs/SourceTarball.md declares them, read here without running that file:
each of its components must be in COMPILED_IN or NOT_COMPILED_IN below, so that one added
there is decided here before this passes, and a component SOURCE_COMPONENTS gives an SPDX
identifier must have that identifier here. Then each field must name:

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
its elements. The Nix package's list is read by nixpkgs' names (NIX_NAMES, each the spdxId
lib/licenses/licenses.nix gives it in the nixpkgs flake.lock pins); a name not there fails.

  python build/check-package-licenses.py              the tree
  python build/check-package-licenses.py --self-test  the negative controls alone

The tree's check runs the negative controls first, so a check that has stopped seeing what
it reads fails too. Exit codes: 0 every field names exactly what it must; 1 a field names
too little or too much, is not there or is there twice, or a component is undecided; 2 a
file could not be read.
"""

import os
import sys

# Nothing is imported by name from this script's own directory, which Python puts first
# on sys.path (build/check-release-chain.py, PATH_GUARD).
sys.path[:] = [p for p in sys.path if os.path.normcase(os.path.realpath(p or os.curdir)) !=
               os.path.normcase(os.path.dirname(os.path.realpath(__file__)))]

import ast
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = "hmailserver/source/Server"
PACKAGING = SERVER + "/platform/packaging"
ASSET_SBOM = "build/asset-sbom.py"
CMAKELISTS = SERVER + "/CMakeLists.txt"

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

READERS = {
    "cpack": re.compile(r'^set\(CPACK_RPM_PACKAGE_LICENSE\s+"([^"\n]*)"\s*\)', re.MULTILINE),
    "spec": re.compile(r"^License:[ \t]*(\S[^\n]*?)[ \t]*$", re.MULTILINE),
    "apkbuild": re.compile(r'^license="([^"\n]*)"[ \t]*$', re.MULTILINE),
    "pkgbuild": re.compile(r"^license=\(([^)\n]*)\)[ \t]*$", re.MULTILINE),
    "nix": re.compile(r"^[ \t]*license = with lib\.licenses; \[([^\]\n]*)\];[ \t]*$", re.MULTILINE),
}
BOOST_OPTION = re.compile(r'^option\(HM_BOOST_STATIC "[^"\n]*" (ON|OFF)\)', re.MULTILINE)
BOOST_OFF = "-DHM_BOOST_STATIC=OFF"
IDENTIFIER = re.compile(r"^(?:LicenseRef-)?[A-Za-z0-9][A-Za-z0-9.+-]*$")


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
        elements = re.findall(r"""'([^']*)'|"([^"]*)\"""", value)
        if re.sub(r"""'[^']*'|"[^"]*\"""", "", value).strip() or not elements:
            return None, "is not an array of quoted SPDX expressions: (%s)" % value
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


def boost_static(kind, text, cmake_default):
    """Whether the build that field is of links Boost statically: the CMake default,
    unless a line of the file that is not a comment passes -DHM_BOOST_STATIC=OFF."""
    if kind == "cpack":
        return cmake_default
    code = [line for line in text.split("\n") if not line.lstrip().startswith("#")]
    return cmake_default and not any(BOOST_OFF in line for line in code)


def problems_of(texts):
    """What is wrong, given {path: text} of the five files, the CMakeLists and asset-sbom.py."""
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


def tree_texts(root):
    paths = [ASSET_SBOM, CMAKELISTS] + [path for _, path, _, _ in FIELDS]
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
}


def self_test():
    failures = []

    def expect(what, texts, wanted):
        try:
            found = problems_of(texts)
        except Unreadable as e:
            found = [str(e)]
        if wanted is None:
            if found:
                failures.append("%s: expected no problem, got %s" % (what, found))
        elif not any(wanted in p for p in found):
            failures.append("%s: expected a problem saying %r, got %s" % (what, wanted, found))

    def changed(path, old, new):
        texts = dict(GOOD)
        if old not in texts[path]:
            failures.append("the fixture of %s has no %r to change" % (path, old))
        texts[path] = texts[path].replace(old, new)
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
    return failures


def main(argv):
    if argv not in ([], ["--self-test"]):
        print("usage: check-package-licenses.py [--self-test]")
        return 2
    failures = self_test()
    if failures:
        print("FAILED: the negative controls - the check no longer sees what it reads:")
        for failure in failures:
            print("  " + failure)
        return 1
    if argv == ["--self-test"]:
        print("OK: the negative controls, each refused as it should be")
        return 0
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
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
