#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""
The two command-line clients' packages are what the tree says they are
(hmailserver/docs/CommandLinePackages.md).

hmctl goes to PyPI as hmailserver-cli, built by build/ci/cli-packages.py from
hmailserver/source/Tools/Cli/python/ and the hmctl file beside it; the PowerShell
module goes to the PowerShell Gallery as HmailServer, with its manifest
HmailServer.psd1. build/check-cli.py tests what the two clients DO against a
recorded server; this checks what they are packaged AS, and fails when:

  the version
    * HmailServer.psd1's ModuleVersion, or hmctl's VERSION, is not
      Common/Application/Version.h's
      version (it is stamped with it at a release, RELEASE.md step 7);
    * python/pyproject.toml writes a version at all: the package's is derived from
      Version.h when it is built, and a literal would be a second place to stamp;

  the PowerShell module's two files
    * HmailServer.psm1 or HmailServer.psd1 holds a byte that is not ASCII - a
      byte order mark among them. A release signs both with Authenticode, and
      Windows PowerShell 5.1 reads a file that has no byte order mark in the
      machine's own ANSI code page, which is what the signature is checked over:
      a character beyond ASCII reads as another character on a machine with
      another code page, and the module's signature no longer matches there.
      ASCII reads the same in every code page;

  the PowerShell manifest
    * FunctionsToExport is not exactly the functions the module's own
      Export-ModuleMember names, or names one the module does not define, or is a
      wildcard; the cmdlets, variables and aliases are not listed as none; the
      module exports anything but functions;
    * RootModule, PowerShellVersion 5.1, CompatiblePSEditions Desktop and Core,
      the GUID, the project and licence addresses are not what they must be;
    * the Description's first sentence does not say that the module is Progressive
      Robot's fork's and not the original hMailServer's, which is developed
      separately and has no REST API;

  the Python package
    * pyproject.toml declares a dependency, an optional one, an entry point other
      than the hmctl console script, a build backend other than flit_core, a flit
      setting that adds files, or a flit_core range that does not hold the version
      build/ci/cli-packages-requirements.txt pins;
    * the summary PyPI lists (pyproject.toml's description) is not one line of at
      most 512 characters, PyPI's limit, or it or the opening of the README PyPI
      shows does not say that hmctl is Progressive Robot's fork's and not the
      original hMailServer's;
    * python/ holds anything but pyproject.toml and README.md - a setup.py or
      setup.cfg (code that runs at install), a .pth file, or a committed copy of
      hmctl, which would be a second hmctl;
    * hmctl imports anything outside the standard library, or a standard library
      module Python 3.8 does not have (zoneinfo, graphlib, tomllib and the later
      ones), fails to parse with Python 3.8's grammar (as far as ast's
      feature_version checks it), has no main(), or runs code when it is imported - the package
      imports it as hmailserver_cli.hmctl, and OpenSSF Package Analysis imports
      every module of every PyPI upload, so an import must define and do nothing
      else (a call at module level other than a few pure constructors, a decorator,
      a loop, a with, a try, an if other than the __main__ guard);

  with --dist DIR, the built sdist and wheel
    * are not exactly one hmailserver_cli-<version>.tar.gz and one
      hmailserver_cli-<version>-py3-none-any.whl at Version.h's version;
    * hold a file beyond the package's three modules, its metadata and its
      licence (a .pth, a data directory, a setup.py), or an hmctl.py that is not
      the tree's hmctl byte for byte, or metadata with a Requires-Dist, another
      version, another licence, or an entry point beyond the hmctl script.

It reads files and needs nothing but Python 3.11 (tomllib); the checkers job of
.gitlab-ci.yml runs it with no arguments. The cli-packages job of
.gitlab/ci/cli-packages.yml runs it with --dist on what it built.

Usage:
  python build/check-cli-packages.py                  the tree
  python build/check-cli-packages.py --dist DIR       the tree, and a built sdist and wheel
  python build/check-cli-packages.py --self-test      each rule broken in a copy, and caught
"""

import argparse
import ast
import configparser
import email.parser
import io
import os
import re
import shutil
import sys
import tarfile
import tempfile
import zipfile

try:
    import tomllib
except ImportError:  # Python before 3.11
    tomllib = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CLI = "hmailserver/source/Tools/Cli"
PYTHON_DIR = CLI + "/python"
HMCTL = CLI + "/hmctl"
PSM1 = CLI + "/HmailServer.psm1"
PSD1 = CLI + "/HmailServer.psd1"
VERSION_H = "hmailserver/source/Server/Common/Application/Version.h"
REQUIREMENTS = "build/ci/cli-packages-requirements.txt"

DIST_NAME = "hmailserver-cli"
MODULE = "hmailserver_cli"
PROJECT_URL = "https://gitlab.com/hmailserver/hmailserver"
LICENSE_EXPRESSION = "AGPL-3.0-or-later"
CONSOLE_SCRIPTS = {"hmctl": "hmailserver_cli.hmctl:main"}
COMMITTED_PYTHON_FILES = {"pyproject.toml", "README.md"}
PACKAGE_MODULES = {"__init__.py", "__main__.py", "hmctl.py"}
MODULE_GUID = "89f008bd-5dc0-4d6d-9a05-4ac1c8dbf8e5"

# What every listing of the two packages says first: the original hMailServer, Martin
# Knafve's, is developed separately and has no REST API, and a package of that name
# that did not say whose it is would read as the original's.
FORK_WORDS = ("Progressive Robot's fork of hMailServer", "not the original hMailServer")
PYPI_SUMMARY_LIMIT = 512  # PyPI refuses a longer Summary, and one with a line break


def says_the_fork(text):
    """Whether text, its line breaks read as spaces, names the fork and says it is not the original."""
    flat = " ".join(str(text).split())
    return all(words in flat for words in FORK_WORDS)

# Calls hmctl may make at module level: constructors with no effect beyond the value.
PURE_CALLS = {"frozenset", "set", "dict", "list", "tuple", "object", "str", "int", "float", "bytes",
              "re.compile", "collections.OrderedDict", "collections.namedtuple"}

# Standard library modules newer than Python 3.8, the floor requires-python states:
# sys.stdlib_module_names of the Python running this check has them, and an import of
# one would pass as the standard library and fail on 3.8. By the version that added it.
NEWER_THAN_FLOOR = {"zoneinfo": "3.9", "graphlib": "3.9", "tomllib": "3.11", "wsgiref.types": "3.12",
                    "annotationlib": "3.14", "compression": "3.14", "concurrent.interpreters": "3.14",
                    "string.templatelib": "3.14"}


def read(root, relative, mode="r"):
    with open(os.path.join(root, relative), mode, **({} if "b" in mode else {"encoding": "utf-8-sig"})) as f:
        return f.read()


def read_version(root):
    text = read(root, VERSION_H)
    m = re.search(r'^#define\s+HMAILSERVER_VERSION\s+"(\d+\.\d+\.\d+)"', text, re.MULTILINE)
    if not m:
        raise ValueError("%s has no three-part HMAILSERVER_VERSION" % VERSION_H)
    return m.group(1)


# ---------------------------------------------------------------- the PowerShell data file

class Psd1Error(ValueError):
    pass


def psd1_tokens(text):
    """The tokens of a PowerShell data file: the literal subset a module manifest is
    written in (hashtables, arrays, quoted strings, numbers, $true, $false, $null)."""
    tokens, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("<#", i):
            end = text.find("#>", i + 2)
            if end < 0:
                raise Psd1Error("a block comment is not closed")
            i = end + 2
        elif c == "#":
            while i < n and text[i] not in "\r\n":
                i += 1
        elif c in " \t\r":
            i += 1
        elif c in "\n;":
            tokens.append(("sep", c))
            i += 1
        elif text.startswith("@{", i):
            tokens.append(("{", "@{"))
            i += 2
        elif text.startswith("@(", i):
            tokens.append(("(", "@("))
            i += 2
        elif c in "}),=":
            tokens.append((c, c))
            i += 1
        elif c == "'":
            value, i = [], i + 1
            while True:
                if i >= n:
                    raise Psd1Error("a string is not closed")
                if text[i] == "'":
                    if text.startswith("''", i):
                        value.append("'")
                        i += 2
                        continue
                    i += 1
                    break
                value.append(text[i])
                i += 1
            tokens.append(("str", "".join(value)))
        elif c == '"':
            value, i = [], i + 1
            while True:
                if i >= n:
                    raise Psd1Error("a string is not closed")
                if text[i] == '"':
                    if text.startswith('""', i):
                        value.append('"')
                        i += 2
                        continue
                    i += 1
                    break
                if text[i] in "$`":
                    raise Psd1Error("a double-quoted string expands $ or `, which a data file cannot")
                value.append(text[i])
                i += 1
            tokens.append(("str", "".join(value)))
        else:
            m = re.compile(r"[$A-Za-z0-9_.\-]+").match(text, i)
            if not m:
                raise Psd1Error("unexpected %r" % c)
            tokens.append(("word", m.group(0)))
            i = m.end()
    return tokens


def parse_psd1(text):
    """A module manifest as Python values: a hashtable is a dict keyed by the
    lower-cased key (PowerShell's keys are not case-sensitive), an array a list."""
    tokens = psd1_tokens(text)
    position = [0]

    def peek():
        return tokens[position[0]] if position[0] < len(tokens) else (None, None)

    def take(kind=None):
        token = peek()
        if kind is not None and token[0] != kind:
            raise Psd1Error("expected %s, found %r" % (kind, token[1]))
        position[0] += 1
        return token

    def skip(kinds=("sep",)):
        while peek()[0] in kinds:
            position[0] += 1

    def scalar():
        kind, value = take()
        if kind == "str":
            return value
        if kind == "word":
            lowered = value.lower()
            if lowered in ("$true", "$false"):
                return lowered == "$true"
            if lowered == "$null":
                return None
            if re.match(r"^-?\d+(\.\d+)?$", value):
                return float(value) if "." in value else int(value)
        raise Psd1Error("%r is not a literal a data file may hold" % value)

    def element():
        """A hashtable, an @() array or a scalar: one item, without a comma list."""
        kind = peek()[0]
        if kind == "{":
            return hashtable()
        if kind == "(":
            take("(")
            items = []
            skip(("sep", ","))
            while peek()[0] != ")":
                if peek()[0] is None:
                    raise Psd1Error("an array is not closed")
                items.append(element())
                skip(("sep", ","))
            take(")")
            return items
        return scalar()

    def value():
        """What follows a key's =: an element, or a comma list of them ('a', 'b'),
        which PowerShell reads as an array."""
        first = element()
        if peek()[0] != ",":
            return first
        items = [first]
        while peek()[0] == ",":
            take(",")
            skip()
            items.append(element())
        return items

    def hashtable():
        take("{")
        table = {}
        skip()
        while peek()[0] != "}":
            kind, key = take()
            if kind not in ("word", "str"):
                raise Psd1Error("expected a key, found %r" % key)
            take("=")
            skip()
            if key.lower() in table:
                raise Psd1Error("the key %s is given twice" % key)
            table[key.lower()] = value()
            skip()
        take("}")
        return table

    skip()
    result = hashtable()
    skip()
    if peek()[0] is not None:
        raise Psd1Error("something follows the manifest's hashtable: %r" % (peek()[1],))
    return result


def psm1_exports(text):
    """(the functions the module's Export-ModuleMember calls name, the problems
    with those calls)."""
    names, problems = [], []
    for m in re.finditer(r"^\s*Export-ModuleMember\b(.*?)(?=^\S|\Z)", text, re.MULTILINE | re.DOTALL):
        call = m.group(1)
        switches = set(s.lower() for s in re.findall(r"(?<![\w-])-(\w+)", call))
        if switches - {"function"}:
            problems.append("%s's Export-ModuleMember exports more than functions (-%s): the manifest lists functions"
                            " alone, and PowerShell would hide the rest" % (PSM1, ", -".join(sorted(switches - {"function"}))))
        names += [a or b for a, b in re.findall(r"'([^']+)'|\"([^\"]+)\"", call)]
    return names, problems


def psm1_functions(text):
    return set(re.findall(r"^function\s+([A-Za-z][\w-]*)", text, re.MULTILINE | re.IGNORECASE))


def check_module_ascii(root):
    """The module's two files, byte by byte: ASCII, and so read the same in every code page."""
    problems = []
    for relative in (PSM1, PSD1):
        path = os.path.join(root, relative)
        if not os.path.isfile(path):
            continue
        data = read(root, relative, "rb")
        found = [i for i, byte in enumerate(data) if byte > 0x7F]
        if not found:
            continue
        first = found[0]
        line = data.count(b"\n", 0, first) + 1
        column = first - (data.rfind(b"\n", 0, first) + 1) + 1
        what = "a byte order mark" if data.startswith(b"\xef\xbb\xbf") and first == 0 else "a byte that is not ASCII"
        problems.append("%s holds %s at line %d, column %d (%d such byte(s) in all): a release signs it, and Windows"
                        " PowerShell 5.1 reads a file without a byte order mark in the machine's own code page, so"
                        " its signature would not match on a machine with another; write the character in ASCII"
                        % (relative, what, line, column, len(found)))
    return problems


def check_manifest(root, version):
    problems = []
    if not os.path.isfile(os.path.join(root, PSD1)):
        return ["%s is missing: the PowerShell Gallery takes a module with a manifest" % PSD1]
    try:
        manifest = parse_psd1(read(root, PSD1))
    except Psd1Error as error:
        return ["%s does not parse as a PowerShell data file: %s" % (PSD1, error)]
    module = read(root, PSM1).replace("\r\n", "\n")

    def field(name):
        return manifest.get(name.lower())

    def as_list(name):
        v = field(name)
        return v if isinstance(v, list) else ([] if v is None else [v])

    if field("ModuleVersion") != version:
        problems.append("%s says ModuleVersion '%s' and %s says %s: the module is stamped with the server's version at"
                        " a release (RELEASE.md, 'Version stamp')" % (PSD1, field("ModuleVersion"), VERSION_H, version))
    if field("RootModule") != "HmailServer.psm1":
        problems.append("%s's RootModule is %r, not 'HmailServer.psm1'" % (PSD1, field("RootModule")))
    if field("GUID") != MODULE_GUID:
        problems.append("%s's GUID is %r: a module keeps the GUID it was first published with (%s)"
                        % (PSD1, field("GUID"), MODULE_GUID))
    if field("PowerShellVersion") != "5.1":
        problems.append("%s's PowerShellVersion is %r, not '5.1'" % (PSD1, field("PowerShellVersion")))
    if sorted(as_list("CompatiblePSEditions")) != ["Core", "Desktop"]:
        problems.append("%s's CompatiblePSEditions is %r, not Desktop and Core" % (PSD1, field("CompatiblePSEditions")))
    for name in ("Author", "Description", "Copyright"):
        if not isinstance(field(name), str) or not field(name).strip():
            problems.append("%s has no %s" % (PSD1, name))
    if isinstance(field("Description"), str):
        first_sentence = field("Description").split(". ")[0]
        if not says_the_fork(first_sentence):
            problems.append("%s's Description does not say in its first sentence that the module is the client of %s, %s: the"
                            " Gallery lists it, and the original is developed separately and has no REST API"
                            " (first sentence: %r)" % (PSD1, FORK_WORDS[0], FORK_WORDS[1], first_sentence))

    listed = as_list("FunctionsToExport")
    exported, export_problems = psm1_exports(module)
    problems += export_problems
    defined = psm1_functions(module)
    if any(not isinstance(n, str) or "*" in n or "?" in n for n in listed):
        problems.append("%s's FunctionsToExport holds a wildcard: the Gallery and PowerShell's command discovery"
                        " read the list as written" % PSD1)
    duplicates = sorted(set(n for n in listed if listed.count(n) > 1))
    if duplicates:
        problems.append("%s's FunctionsToExport names %s twice" % (PSD1, ", ".join(duplicates)))
    missing = sorted(set(exported) - set(listed))
    extra = sorted(set(listed) - set(exported))
    if missing:
        problems.append("%s exports %s, which %s's FunctionsToExport does not list" % (PSM1, ", ".join(missing), PSD1))
    if extra:
        problems.append("%s's FunctionsToExport lists %s, which %s does not export" % (PSD1, ", ".join(extra), PSM1))
    undefined = sorted(set(n for n in listed if isinstance(n, str)) - defined)
    if undefined:
        problems.append("%s's FunctionsToExport lists %s, which %s does not define" % (PSD1, ", ".join(undefined), PSM1))
    if not exported:
        problems.append("%s names no function in an Export-ModuleMember call" % PSM1)
    for name in ("CmdletsToExport", "VariablesToExport", "AliasesToExport"):
        if field(name) != []:
            problems.append("%s's %s is %r, not @(): the module exports functions alone, and a missing key means"
                            " '*' to PowerShell" % (PSD1, name, field(name)))

    data = (field("PrivateData") or {}).get("psdata") if isinstance(field("PrivateData"), dict) else None
    if not isinstance(data, dict):
        problems.append("%s has no PrivateData.PSData, where the Gallery reads the project's addresses" % PSD1)
    else:
        if data.get("projecturi") != PROJECT_URL:
            problems.append("%s's ProjectUri is %r, not %s" % (PSD1, data.get("projecturi"), PROJECT_URL))
        if not str(data.get("licenseuri") or "").startswith(PROJECT_URL + "/"):
            problems.append("%s's LicenseUri %r is not the project's" % (PSD1, data.get("licenseuri")))
        if not isinstance(data.get("tags"), list) or not data.get("tags"):
            problems.append("%s has no Tags" % PSD1)
    return problems


# ---------------------------------------------------------------- the Python package

def check_python_dir(root):
    problems = []
    base = os.path.join(root, PYTHON_DIR)
    if not os.path.isdir(base):
        return ["%s is missing" % PYTHON_DIR]
    found = set()
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [d for d in dirnames if d != "__pycache__"]
        for name in filenames:
            found.add(os.path.relpath(os.path.join(dirpath, name), base).replace(os.sep, "/"))
    for name in sorted(found - COMMITTED_PYTHON_FILES):
        if name in ("setup.py", "setup.cfg") or name.endswith(".pth"):
            why = "code or a path hook that runs when the package is installed"
        elif os.path.basename(name) == "hmctl.py" or name.startswith(MODULE + "/"):
            why = ("the package's modules are made when it is built (build/ci/cli-packages.py); a committed copy of"
                   " hmctl is a second hmctl")
        else:
            why = "not a file the package is made from"
        problems.append("%s/%s: %s" % (PYTHON_DIR, name, why))
    for name in sorted(COMMITTED_PYTHON_FILES - found):
        problems.append("%s/%s is missing" % (PYTHON_DIR, name))
    return problems


def pinned_version(root, package):
    try:
        text = read(root, REQUIREMENTS)
    except OSError:
        return None
    m = re.search(r"^%s==(\S+)" % re.escape(package), text, re.MULTILINE | re.IGNORECASE)
    return m.group(1) if m else None


def version_tuple(text):
    return tuple(int(p) for p in re.findall(r"\d+", text)[:3])


def range_holds(spec, version):
    """Whether a requirement's >=, <, ==, != clauses admit version (numeric releases)."""
    v = version_tuple(version)
    for op, bound in re.findall(r"(>=|<=|==|!=|<|>)\s*([0-9][0-9.]*)", spec):
        b = version_tuple(bound)
        b = b + (0,) * (len(v) - len(b))
        if not {">=": v >= b, "<=": v <= b, "==": v == b, "!=": v != b, "<": v < b, ">": v > b}[op]:
            return False
    return True


def check_pyproject(root):
    path = PYTHON_DIR + "/pyproject.toml"
    if tomllib is None:
        return ["this check reads %s with tomllib, which needs Python 3.11 or later" % path]
    try:
        data = tomllib.loads(read(root, path))
    except (OSError, ValueError) as error:
        return ["%s does not parse: %s" % (path, error)]
    problems = []
    system = data.get("build-system", {})
    project = data.get("project", {})
    if system.get("build-backend") != "flit_core.buildapi":
        problems.append("%s's build backend is %r: the package is built by flit_core, which runs no code of the"
                        " project's at build or install time" % (path, system.get("build-backend")))
    requires = system.get("requires", [])
    if len(requires) != 1 or not re.match(r"^flit_core\b", requires[0].replace("-", "_")):
        problems.append("%s's build requires %r, not flit_core alone" % (path, requires))
    else:
        pinned = pinned_version(root, "flit-core") or pinned_version(root, "flit_core")
        if pinned and not range_holds(requires[0], pinned):
            problems.append("%s requires %s, which does not hold flit-core %s, the version %s pins and the jobs"
                            " build with" % (path, requires[0], pinned, REQUIREMENTS))
    if project.get("name") != DIST_NAME:
        problems.append("%s's project name is %r, not %s" % (path, project.get("name"), DIST_NAME))
    if "version" in project:
        problems.append("%s writes version = %r: the version is Version.h's, set when the package is built,"
                        " and never written here" % (path, project["version"]))
    if project.get("dynamic") != ["version"]:
        problems.append("%s's dynamic is %r, not [\"version\"]" % (path, project.get("dynamic")))
    if project.get("dependencies", []) != []:
        problems.append("%s declares dependencies %r: hmctl is the standard library alone" % (path, project["dependencies"]))
    for key in ("optional-dependencies", "gui-scripts", "entry-points"):
        if project.get(key):
            problems.append("%s declares [project.%s]: the package is the hmctl command and nothing else" % (path, key))
    if project.get("scripts") != CONSOLE_SCRIPTS:
        problems.append("%s's scripts are %r, not %r" % (path, project.get("scripts"), CONSOLE_SCRIPTS))
    if project.get("license") != LICENSE_EXPRESSION:
        problems.append("%s's licence is %r, not %s" % (path, project.get("license"), LICENSE_EXPRESSION))
    if project.get("license-files") != ["LICENSE"]:
        problems.append("%s's license-files is %r, not [\"LICENSE\"]" % (path, project.get("license-files")))
    if not project.get("requires-python"):
        problems.append("%s has no requires-python" % path)
    summary = project.get("description")
    if not isinstance(summary, str) or not summary.strip():
        problems.append("%s has no description, the summary PyPI lists" % path)
    else:
        if "\n" in summary or "\r" in summary or len(summary) > PYPI_SUMMARY_LIMIT:
            problems.append("%s's description is not one line of at most %d characters, which is all PyPI takes as"
                            " a summary (%d characters)" % (path, PYPI_SUMMARY_LIMIT, len(summary)))
        if not says_the_fork(summary):
            problems.append("%s's description does not say that hmctl is the client of %s, %s: PyPI lists it as the summary,"
                            " and the original is developed separately and has no REST API (%r)"
                            % (path, FORK_WORDS[0], FORK_WORDS[1], summary))
    if project.get("readme") != "README.md":
        problems.append("%s's readme is %r, not README.md" % (path, project.get("readme")))
    else:
        try:
            readme = read(root, PYTHON_DIR + "/README.md").replace("\r\n", "\n")
        except OSError as error:
            # A README.md that is not there at all is reported by check_python_dir.
            if os.path.exists(os.path.join(root, PYTHON_DIR, "README.md")):
                problems.append("%s/README.md, the description PyPI shows, cannot be read: %s" % (PYTHON_DIR, error))
        else:
            if not says_the_fork(readme.split("\nInstall\n")[0]):
                problems.append("%s/README.md, the description PyPI shows, does not say before its Install section"
                                " that hmctl is the client of %s, %s" % (PYTHON_DIR, FORK_WORDS[0], FORK_WORDS[1]))
    urls = project.get("urls", {})
    if not urls or any(not str(u).startswith(PROJECT_URL) for u in urls.values()):
        problems.append("%s's project URLs are not all the project's (%s): %r" % (path, PROJECT_URL, urls))
    flit = data.get("tool", {}).get("flit", {})
    if flit.get("module", {}).get("name") != MODULE or set(flit) - {"module"}:
        problems.append("%s's [tool.flit] is %r: the module's name, %s, and nothing that adds a file" % (path, flit, MODULE))
    return problems


def call_name(node):
    parts = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
        return ".".join(reversed(parts))
    return None


def import_time_problems(statements, where, in_class=False):
    """Code among these statements that would run when the module is imported,
    beyond defining names."""
    problems = []

    def calls_in(*nodes):
        found = []
        for node in nodes:
            if node is None:
                continue
            for child in ast.walk(node):
                if isinstance(child, ast.Call) and call_name(child.func) not in PURE_CALLS:
                    found.append(child)
        return found

    def report(node, what):
        problems.append("%s line %d: %s runs when the module is imported" % (where, node.lineno, what))

    for i, node in enumerate(statements):
        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant) and isinstance(node.value.value, str):
            continue
        if isinstance(node, (ast.Import, ast.ImportFrom, ast.Pass)):
            continue
        if isinstance(node, (ast.Assign, ast.AnnAssign, ast.AugAssign, ast.Expr)):
            calls = calls_in(node.value, getattr(node, "annotation", None))
            for call in calls:
                report(node, "a call to %s()" % (call_name(call.func) or "an expression"))
            if isinstance(node, ast.Expr) and not calls:
                report(node, "an expression statement")
            continue
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.decorator_list:
                report(node, "the decorator of %s" % node.name)
            arguments = node.args
            for call in calls_in(*(arguments.defaults + [d for d in arguments.kw_defaults if d is not None])):
                report(node, "a call in %s's default arguments" % node.name)
            continue
        if isinstance(node, ast.ClassDef):
            if node.decorator_list:
                report(node, "the decorator of %s" % node.name)
            for call in calls_in(*(node.bases + [k.value for k in node.keywords])):
                report(node, "a call in %s's bases" % node.name)
            problems += import_time_problems(node.body, where, in_class=True)
            continue
        if (not in_class and isinstance(node, ast.If) and isinstance(node.test, ast.Compare)
                and isinstance(node.test.left, ast.Name) and node.test.left.id == "__name__"
                and len(node.test.ops) == 1 and isinstance(node.test.ops[0], ast.Eq)
                and len(node.test.comparators) == 1 and isinstance(node.test.comparators[0], ast.Constant)
                and node.test.comparators[0].value == "__main__" and not node.orelse):
            continue
        report(node, "a %s statement" % type(node).__name__.lower())
    return problems


def check_hmctl(root, version=None):
    problems = []
    try:
        source = read(root, HMCTL, "rb")
    except OSError:
        return ["%s is missing" % HMCTL]
    if version is None:
        try:
            version = read_version(root)
        except (OSError, ValueError):
            version = None
    if b"\r\n" in source:
        problems.append("%s has CRLF line endings: it is checked out LF on every platform (.gitattributes), and the"
                        " package and /usr/bin/hmctl carry it as it is" % HMCTL)
    try:
        tree = ast.parse(source.decode("utf-8"), filename=HMCTL, feature_version=(3, 8))
    except SyntaxError as error:
        return problems + ["%s does not parse as Python 3.8 (line %s): %s; the package says requires-python >= 3.8"
                           % (HMCTL, error.lineno, error.msg)]
    stdlib = getattr(sys, "stdlib_module_names", None)
    for node in ast.walk(tree):
        names = []
        if isinstance(node, ast.Import):
            names = [a.name for a in node.names]
        elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
            names = [node.module] + [node.module + "." + a.name for a in node.names]
        for name in names:
            newer = [m for m in NEWER_THAN_FLOOR if name == m or name.startswith(m + ".")]
            if newer:
                problems.append("%s line %d imports %s, which Python's standard library has from %s only: the package"
                                " says requires-python >= 3.8" % (HMCTL, node.lineno, newer[0], NEWER_THAN_FLOOR[newer[0]]))
                break
            if stdlib is not None and name.split(".")[0] not in stdlib and not (
                    isinstance(node, ast.ImportFrom) and name != node.module):
                problems.append("%s line %d imports %s, which is not in Python's standard library: the package"
                                " declares no dependency" % (HMCTL, node.lineno, name))
    if not any(isinstance(n, ast.FunctionDef) and n.name == "main" for n in tree.body):
        problems.append("%s has no main(): the console script calls hmailserver_cli.hmctl:main" % HMCTL)
    # hmctl --version and its User-Agent say the server's version, stamped at a release.
    said = [n.value.value for n in tree.body if isinstance(n, ast.Assign) and len(n.targets) == 1
            and isinstance(n.targets[0], ast.Name) and n.targets[0].id == "VERSION" and isinstance(n.value, ast.Constant)]
    if version is not None and said != [version]:
        problems.append("%s says VERSION %s and %s says %s: hmctl is stamped with the server's version at a release"
                        " (RELEASE.md, 'Version stamp')" % (HMCTL, " and ".join(repr(s) for s in said) or "nothing", VERSION_H,
                                                             version))
    problems += import_time_problems(tree.body, HMCTL)
    return problems


def check_dist(dist, version, hmctl):
    """The sdist and the wheel in `dist`, held to the version and to hmctl's bytes."""
    problems = []
    prefix = "%s-%s" % (MODULE, version)
    wheel_name = prefix + "-py3-none-any.whl"
    sdist_name = prefix + ".tar.gz"
    present = sorted(n for n in os.listdir(dist) if not n.endswith(".publish.attestation"))
    if present != sorted([sdist_name, wheel_name]):
        return ["%s holds %s, not %s and %s" % (dist, present, sdist_name, wheel_name)]

    with zipfile.ZipFile(os.path.join(dist, wheel_name)) as wheel:
        names = set(wheel.namelist())
        info = prefix + ".dist-info/"
        expected = set(MODULE + "/" + m for m in PACKAGE_MODULES) | set(
            info + n for n in ("METADATA", "WHEEL", "RECORD", "entry_points.txt", "licenses/LICENSE"))
        for name in sorted(names - expected):
            problems.append("the wheel holds %s, which is none of the package's files" % name)
        for name in sorted(expected - names):
            problems.append("the wheel has no %s" % name)
        if MODULE + "/hmctl.py" in names and wheel.read(MODULE + "/hmctl.py") != hmctl:
            problems.append("the wheel's hmctl.py is not %s byte for byte" % HMCTL)
        if info + "METADATA" in names:
            metadata = email.parser.Parser().parsestr(wheel.read(info + "METADATA").decode("utf-8"))
            if metadata.get("Name") != DIST_NAME or metadata.get("Version") != version:
                problems.append("the wheel's metadata names %s %s, not %s %s" % (
                    metadata.get("Name"), metadata.get("Version"), DIST_NAME, version))
            if metadata.get_all("Requires-Dist"):
                problems.append("the wheel requires %s: hmctl is the standard library alone"
                                % ", ".join(metadata.get_all("Requires-Dist")))
            if metadata.get("License-Expression") != LICENSE_EXPRESSION:
                problems.append("the wheel's License-Expression is %r" % metadata.get("License-Expression"))
        if info + "entry_points.txt" in names:
            parser = configparser.ConfigParser(delimiters=("=",))
            parser.optionxform = str
            parser.read_string(wheel.read(info + "entry_points.txt").decode("utf-8"))
            points = dict((s, dict(parser.items(s))) for s in parser.sections())
            if points != {"console_scripts": CONSOLE_SCRIPTS}:
                problems.append("the wheel's entry points are %r, not the hmctl script alone" % points)
        if MODULE + "/__init__.py" in names:
            init = wheel.read(MODULE + "/__init__.py").decode("utf-8")
            if '__version__ = "%s"' % version not in init:
                problems.append("the wheel's %s/__init__.py does not say __version__ = \"%s\"" % (MODULE, version))

    with tarfile.open(os.path.join(dist, sdist_name)) as sdist:
        members = dict((m.name, m) for m in sdist.getmembers() if m.isfile())
        expected = set(prefix + "/" + n for n in ("pyproject.toml", "PKG-INFO", "README.md", "LICENSE")) | set(
            prefix + "/" + MODULE + "/" + m for m in PACKAGE_MODULES)
        for name in sorted(set(members) - expected):
            problems.append("the sdist holds %s, which is none of the package's files" % name)
        for name in sorted(expected - set(members)):
            problems.append("the sdist has no %s" % name)
        member = members.get(prefix + "/" + MODULE + "/hmctl.py")
        if member is not None and sdist.extractfile(member).read() != hmctl:
            problems.append("the sdist's hmctl.py is not %s byte for byte" % HMCTL)
    return problems


# ---------------------------------------------------------------- the run

def run(root, dist=None):
    problems = []
    try:
        version = read_version(root)
    except (OSError, ValueError) as error:
        return [str(error)]
    problems += check_module_ascii(root)
    problems += check_manifest(root, version)
    problems += check_python_dir(root)
    problems += check_pyproject(root)
    problems += check_hmctl(root)
    if dist:
        problems += check_dist(dist, version, read(root, HMCTL, "rb"))
    return problems


def copy_tree(work):
    tree = os.path.join(work, "tree")
    for relative in [VERSION_H, HMCTL, PSM1, PSD1, REQUIREMENTS, PYTHON_DIR + "/pyproject.toml",
                     PYTHON_DIR + "/README.md"]:
        target = os.path.join(tree, relative)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(os.path.join(ROOT, relative), target)
    return tree


def fake_dist(where, version, hmctl, extra_wheel=None, requires=None):
    """A wheel and an sdist shaped as flit_core makes them, in memory."""
    os.makedirs(where, exist_ok=True)
    prefix = "%s-%s" % (MODULE, version)
    info = prefix + ".dist-info/"
    metadata = "Metadata-Version: 2.4\nName: %s\nVersion: %s\nLicense-Expression: %s\n" % (
        DIST_NAME, version, LICENSE_EXPRESSION)
    if requires:
        metadata += "Requires-Dist: %s\n" % requires
    init = '"""x"""\n__version__ = "%s"\n' % version
    with zipfile.ZipFile(os.path.join(where, prefix + "-py3-none-any.whl"), "w") as wheel:
        wheel.writestr(MODULE + "/__init__.py", init)
        wheel.writestr(MODULE + "/__main__.py", "")
        wheel.writestr(MODULE + "/hmctl.py", hmctl)
        wheel.writestr(info + "METADATA", metadata)
        wheel.writestr(info + "WHEEL", "Wheel-Version: 1.0\n")
        wheel.writestr(info + "RECORD", "")
        wheel.writestr(info + "entry_points.txt", "[console_scripts]\nhmctl=hmailserver_cli.hmctl:main\n")
        wheel.writestr(info + "licenses/LICENSE", "AGPL")
        for name, data in (extra_wheel or {}).items():
            wheel.writestr(name, data)
    with tarfile.open(os.path.join(where, prefix + ".tar.gz"), "w:gz") as sdist:
        for name, data in [("pyproject.toml", b""), ("PKG-INFO", metadata.encode()), ("README.md", b""),
                           ("LICENSE", b""), (MODULE + "/__init__.py", init.encode()),
                           (MODULE + "/__main__.py", b""), (MODULE + "/hmctl.py", hmctl)]:
            member = tarfile.TarInfo(prefix + "/" + name)
            member.size = len(data)
            sdist.addfile(member, io.BytesIO(data))


def self_test():
    failures = []
    work = tempfile.mkdtemp(prefix="hm-check-cli-packages-")
    try:
        tree = copy_tree(work)
        version = read_version(tree)
        hmctl = read(tree, HMCTL, "rb")
        clean = run(tree)
        print("%s  the tree as it is" % ("ok  " if not clean else "FAIL"))
        if clean:
            failures.append("the tree as it is: " + "; ".join(clean))
        fake_dist(os.path.join(work, "dist"), version, hmctl)
        clean_dist = run(tree, os.path.join(work, "dist"))
        print("%s  a well-formed sdist and wheel" % ("ok  " if not clean_dist else "FAIL"))
        if clean_dist:
            failures.append("a well-formed sdist and wheel: " + "; ".join(clean_dist))

        def broken(what, relative, old, new, needle, dist=None):
            path = os.path.join(tree, relative)
            with open(path, "rb") as f:
                held = f.read()
            if old.encode() not in held.replace(b"\r\n", b"\n"):
                print("FAIL  %s: the text to break is not there" % what)
                failures.append(what)
                return
            with open(path, "wb") as f:
                f.write(held.replace(b"\r\n", b"\n").replace(old.encode(), new.encode(), 1))
            try:
                found = run(tree, dist)
            finally:
                with open(path, "wb") as f:
                    f.write(held)
            caught = any(needle in p for p in found)
            print("%s  %s" % ("ok  " if caught else "FAIL", what))
            if not caught:
                failures.append("%s (found: %s)" % (what, "; ".join(found) or "nothing"))

        broken("the manifest's version behind Version.h", PSD1, "ModuleVersion        = '%s'" % version,
               "ModuleVersion        = '0.0.1'", "ModuleVersion '0.0.1'")
        broken("hmctl's version behind Version.h", HMCTL, 'VERSION = "%s"' % version, 'VERSION = "1.0"',
               "says VERSION '1.0'")
        broken("a function exported and not listed", PSD1, "        'Get-HmDomain',\n", "",
               "FunctionsToExport does not list")
        broken("a function listed and not exported", PSD1, "'Add-HmGroupMember',", "'Add-HmGroupMember', 'Get-HmNothing',",
               "which hmailserver/source/Tools/Cli/HmailServer.psm1 does not export")
        broken("a wildcard export", PSD1, "'Add-HmGroupMember',", "'Add-HmGroupMember', 'Get-*',", "wildcard")
        broken("aliases left to '*'", PSD1, "    AliasesToExport      = @()\n", "", "AliasesToExport is None")
        broken("Windows PowerShell 5.1 dropped", PSD1, "@('Desktop', 'Core')", "@('Core')", "CompatiblePSEditions")
        broken("another project's address", PSD1, "ProjectUri   = 'https://gitlab.com/hmailserver/hmailserver'",
               "ProjectUri   = 'https://example.com/x'", "ProjectUri")
        broken("an alias exported by the module", PSM1, "Export-ModuleMember -Function @(",
               "Export-ModuleMember -Alias x -Function @(", "exports more than functions")
        broken("a character beyond ASCII in the module", PSM1, "# https://www.progressiverobot.com\n",
               "# https://www.progressiverobot.com \u2014 the module\n", "HmailServer.psm1 holds a byte that is not ASCII at line 1")
        broken("a character beyond ASCII in the manifest", PSD1, "Author               = 'Christopher Holloway",
               "Author               = 'Chr\u00edstopher Holloway", "HmailServer.psd1 holds a byte that is not ASCII")
        broken("a byte order mark on the module", PSM1, "# https://www.progressiverobot.com\n",
               "\ufeff# https://www.progressiverobot.com\n", "HmailServer.psm1 holds a byte order mark at line 1, column 1")
        broken("a version written in pyproject.toml", PYTHON_DIR + "/pyproject.toml", 'dynamic = ["version"]',
               'version = "6.3.5"', "writes version")
        broken("a dependency", PYTHON_DIR + "/pyproject.toml", "dependencies = []", 'dependencies = ["requests"]',
               "declares dependencies")
        broken("another build backend", PYTHON_DIR + "/pyproject.toml", 'build-backend = "flit_core.buildapi"',
               'build-backend = "setuptools.build_meta"', "build backend")
        broken("a flit_core range the pin is outside", PYTHON_DIR + "/pyproject.toml", '"flit_core >=4.1,<5"',
               '"flit_core >=3.2,<4"', "does not hold flit-core")
        broken("a second entry point", PYTHON_DIR + "/pyproject.toml", 'hmctl = "hmailserver_cli.hmctl:main"',
               'hmctl = "hmailserver_cli.hmctl:main"\nhmctl2 = "x:y"', "scripts are")
        broken("a URL elsewhere", PYTHON_DIR + "/pyproject.toml", 'Issues = "https://gitlab.com/hmailserver/hmailserver/-/issues"',
               'Issues = "https://github.com/x/y/issues"', "project URLs")
        summary = 'description = "hmctl, the command-line client of Progressive Robot\'s fork of hMailServer'
        broken("a PyPI summary that does not say whose hmctl is", PYTHON_DIR + "/pyproject.toml", summary,
               'description = "hmctl: administer an hMailServer over its REST API from a command line"\nx = "',
               "description does not say that hmctl is the client of Progressive Robot's fork")
        broken("a PyPI summary of two lines", PYTHON_DIR + "/pyproject.toml", summary,
               'description = "hmctl\\n, the command-line client of Progressive Robot\'s fork of hMailServer',
               "is not one line of at most 512 characters")
        broken("a PyPI summary longer than PyPI takes", PYTHON_DIR + "/pyproject.toml", summary,
               summary + " " + "x" * 512, "is not one line of at most 512 characters")
        broken("a README that does not say whose hmctl is", PYTHON_DIR + "/README.md",
               "[Progressive Robot's fork of hMailServer](", "[hMailServer](",
               "README.md, the description PyPI shows, does not say")
        broken("a module Description that does not say whose the module is", PSD1,
               "'The PowerShell client of Progressive Robot''s fork of hMailServer,",
               "'Administer an hMailServer. The PowerShell client of Progressive Robot''s fork of hMailServer,",
               "Description does not say in its first sentence")
        broken("a module-level call in hmctl", HMCTL, "\nVERSION = ", "\nos.system('true')\nVERSION = ",
               "a call to os.system() runs when the module is imported")
        broken("a third-party import in hmctl", HMCTL, "\nimport argparse\n", "\nimport argparse\nimport requests\n",
               "imports requests")
        broken("a standard library module newer than 3.8 in hmctl", HMCTL, "\nimport argparse\n",
               "\nimport argparse\nimport tomllib\n", "imports tomllib, which Python's standard library has from 3.11")
        broken("one imported inside a function, by from", HMCTL, "\ndef main(argv=None):\n",
               "\ndef main(argv=None):\n    from zoneinfo import ZoneInfo\n", "imports zoneinfo")
        broken("a Python 3.10 construct in hmctl", HMCTL, "\nVERSION = ", "\nmatch EXIT_OK:\n    case _:\n        pass\nVERSION = ",
               "does not parse as Python 3.8")
        broken("an inverted __main__ guard in hmctl", HMCTL, 'if __name__ == "__main__":', 'if __name__ != "__main__":',
               "a if statement runs when the module is imported")
        broken("hmctl without main()", HMCTL, "\ndef main(argv=None):", "\ndef main_(argv=None):", "has no main()")
        broken("a Version.h the package's metadata no longer matches", VERSION_H,
               '"%s"' % version, '"9.9.9"', "not hmailserver_cli-9.9.9.tar.gz", dist=os.path.join(work, "dist"))

        for name, content, needle in [("setup.py", "import os\n", "runs when the package is installed"),
                                      ("hmailserver_cli/hmctl.py", "", "a second hmctl"),
                                      ("x.pth", "import os\n", "runs when the package is installed")]:
            path = os.path.join(tree, PYTHON_DIR, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(content)
            found = run(tree)
            os.remove(path)
            caught = any(needle in p and name in p for p in found)
            print("%s  a committed %s" % ("ok  " if caught else "FAIL", name))
            if not caught:
                failures.append("a committed %s" % name)
        shutil.rmtree(os.path.join(tree, PYTHON_DIR, MODULE), ignore_errors=True)

        readme_path = os.path.join(tree, PYTHON_DIR, "README.md")
        with open(readme_path, "rb") as f:
            held = f.read()
        os.remove(readme_path)
        try:
            found = run(tree)
            caught = "%s/README.md is missing" % PYTHON_DIR in found
        except Exception as error:
            found = ["%s: %s" % (type(error).__name__, error)]
            caught = False
        finally:
            with open(readme_path, "wb") as f:
                f.write(held)
        print("%s  a python/ without README.md" % ("ok  " if caught else "FAIL"))
        if not caught:
            failures.append("a python/ without README.md (found: %s)" % ("; ".join(found) or "nothing"))

        for what, kwargs, needle in [
                ("a .pth in the wheel", {"extra_wheel": {"hmailserver_cli.pth": "import os"}}, "hmailserver_cli.pth"),
                ("a dependency in the wheel", {"requires": "requests"}, "the wheel requires requests"),
                ("another hmctl in the wheel", {"hmctl_other": True}, "not hmailserver/source/Tools/Cli/hmctl byte")]:
            where = os.path.join(work, "dist-" + what.replace(" ", "-"))
            built = hmctl + b"#" if kwargs.pop("hmctl_other", False) else hmctl
            fake_dist(where, version, built, **kwargs)
            found = run(tree, where)
            caught = any(needle in p for p in found)
            print("%s  %s" % ("ok  " if caught else "FAIL", what))
            if not caught:
                failures.append("%s (found: %s)" % (what, "; ".join(found) or "nothing"))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("")
    if failures:
        print("%d self-test case(s) failed:" % len(failures))
        for f in failures:
            print("  " + f)
        return 1
    print("Every self-test case passed: each rule, broken, is reported.")
    return 0


def main():
    parser = argparse.ArgumentParser(description="The command-line clients' packages are what the tree says.")
    parser.add_argument("--dist", help="a directory holding a built sdist and wheel, checked too")
    parser.add_argument("--root", default=ROOT, help=argparse.SUPPRESS)
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    problems = run(arguments.root, arguments.dist)
    try:
        version = read_version(arguments.root)
    except (OSError, ValueError):
        version = "?"
    if problems:
        for p in problems:
            print("::error::" + p)
        print("")
        print("%d problem(s) with the command-line packages." % len(problems))
        return 1
    print("HmailServer.psd1 is version %s with the module's %d functions; hmailserver-cli is built from hmctl at"
          " Version.h's version, with no dependency and no install-time code%s."
          % (version, len(psm1_exports(read(arguments.root, PSM1).replace("\r\n", "\n"))[0]),
             "; the sdist and the wheel in %s agree" % arguments.dist if arguments.dist else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
