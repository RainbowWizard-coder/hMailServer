#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A change that adds a dependency version with a known High or Critical advisory is held.

The mr-dependency-check job of .gitlab/ci/mr-checks.yml runs this on every change
before it lands: in a merge request's pipeline, and in a protected batch* branch's
pipeline, which is the one GitLab holds that branch's merge request to; on GitLab's
shared runners, with no secret. The dependency-scan job (Trivy and OSV,
build/ci/dependency-scan.py) reads the whole tree, but on linix, for a protected
ref only, and is allow_failure, so that a finding no change brought does not stop
an unrelated one: this is the check of what the change itself adds (Roadmap3,
"Dependency check on merge requests"; OpenSSF Baseline OSPS-VM-05.03). What GitHub's
dependency review did until 18 September 2026.

The change's base is build/ci/security-gate.py's change_base (--base auto):
CI_MERGE_REQUEST_DIFF_BASE_SHA, the target branch where the merge request left it,
in a merge request's pipeline; in a batch* branch's, where HEAD leaves master,
fetched by name. What it reads, at the base and at the head, from git:

  **/packages*.lock.json   NuGet's lock files - the tools', the Control Panel's, the
                           tests' - and the ARM64 ones beside them
                           (packages.arm64.lock.json), or under lockfiles/ should
                           they move there: every resolved package that is not a
                           project of the tree;
  **/packages.config       the older .NET projects' NuGet packages;
  **/*requirements*.txt    the Python tools' pinned packages (name==version), PyPI -
                           every name Renovate's pip_requirements manager updates,
                           build/ci/cli-packages-requirements.txt among them; a line
                           the change adds that pins no version so (a range, a URL
                           or a path, an editable, another index, a requirements
                           file the tree does not hold, a ${...} pip expands) holds
                           it, since pip would install what OSV cannot be asked
                           about;
  the native and vendored libraries
                           every library of build/ci/dependency-scan.py's VENDORED,
                           at each version its file pins, read as that scan reads
                           it: OpenSSL, Boost and libpq from hMailServer.vcxproj's
                           paths, zlib from zlib.h, QuickJS from its VERSION, and
                           MariaDB Connector/C, 7-Zip and SQL Server Compact from
                           third-party-binaries.json; and
  **/native-dependencies*.json
                           a native component list (build/native-dependencies.ps1),
                           if one is committed, whose components are taken as the
                           library of VENDORED with the same purl name.

A dependency is added when its ecosystem, name and version are at the head and
nowhere at the base: an upgrade adds its new version, a new lock file adds all of
its own. Each added one is looked up in OSV's own export of every advisory of its
ecosystem, https://osv-vulnerabilities.storage.googleapis.com/<ecosystem>/all.zip -
the export build/ci/osv-offline-db.py builds Renovate's database from, fetched the
same way; nothing is fetched from GitHub. An advisory affects the version when its
"versions" list names it, or one of its ECOSYSTEM or SEMVER ranges holds it,
compared as the ecosystem orders versions (NuGet's SemVer, PyPI's PEP 440); a
withdrawn advisory affects nothing.

Its severity is, in order: Critical for a malicious package (a MAL- advisory, the
OpenSSF malicious-packages data OSV carries); the GitHub advisory database's rating
in database_specific.severity (CRITICAL, HIGH, MODERATE, LOW); the score of its CVSS
v3 vector, else of its CVSS v4.0 vector, as FIRST's specifications compute them; the
highest of those of its aliases in the same export. One with none of those is listed
as unrated and holds nothing, as the release's gate does not hold one (SECURITY.md's
threshold is High).

An advisory this cannot decide - the added version, or a version bounding one of the
advisory's ranges, is one it cannot order as the ecosystem does, and no "versions"
list names the added one - is not passed over: rated Critical or High, it holds the
merge request as one that may affect the version, which a maintainer reads and, if it
does not, accepts.

A native or vendored library's version that the head pins and the base does not is
asked of OSV (api.osv.dev) by its upstream repository's tag, as dependency-scan asks,
with the library's CONTROL version beside it, and rated as above, an alias's record
fetched from api.osv.dev when the advisory has no rating of its own. A version older
than the repository's first tag (7-Zip's 22.01), which OSV cannot match, is held by
the Critical and High advisories of that first tag, as ones that may affect it; and a
version older than its control that OSV names nothing for, while it names advisories
for the control (OpenSSL's 0.9.6 draws none where 4.0.0 draws 43), by the control's. A
control VENDORED marks as one OSV is known to name (control_draws) that draws nothing
is OSV's data not read: exit 2. A library with no repository OSV knows (SQL Server
Compact), or one whose control OSV is not known to name and names nothing for either,
has no usable data: it is listed as UNKNOWN and not held, and is read by hand against
the SBOM at release (SECURITY.md). So is a component of a native list that is no
library of VENDORED.

A Critical or High advisory against an added version holds the change, unless one of
two declarations as the BASE has them accepts it: .gitlab/security-exceptions.yml,
an entry of scan dependency-scan naming the advisory (or one of its aliases) and the
package (a native library by its VENDORED name, as dependency-scan reports it), read
by build/ci/security-gate.py's own reader; or the VEX document,
.gitlab/hmailserver.openvex.json, a not_affected statement naming the advisory and
the package at that version by its purl (pkg:nuget/, pkg:pypi/ with PEP 503's names,
pkg:generic/ for a native library). Never the change's own copies, so a change
cannot accept its own advisory. The rest is listed. A base whose own files are broken
holds no change by that alone, or no change could mend it: its exceptions file that
cannot be read, and an entry of it reviewed after today, accept nothing, and a lock
file of its that cannot be read names nothing, so that every version the head's copy
names is looked up. When the change alters the gates' code, the base's copy of this
script judges it too (build/ci/security-gate.py, base_copy_verdict).

Usage, from the root of a clone of the change's head with its history:
  python3 build/ci/mr-dependency-check.py --base SHA|auto [--osv-dir DIR] [--today YYYY-MM-DD]
  python3 build/ci/mr-dependency-check.py --self-test
--osv-dir reads <ecosystem>.zip from DIR instead of downloading it. Exits 0 when the
change may land, 1 when an added version has an advisory that holds it, 2 when the
base, the lock files, the pins or the advisories cannot be read (never a pass; the
job retries it). OSV's export is fetched only when the change adds a NuGet or PyPI
version, and api.osv.dev asked only when it moves a native library's pin.
"""

import os
import sys

# Nothing is imported by name from this script's own directory, which Python puts first
# on sys.path: a file put there named as one of Python's modules (json.py, re.py) would
# run as this script's code in a change that leaves the script alone
# (build/check-release-chain.py, PATH_GUARD).
sys.path[:] = [p for p in sys.path if os.path.normcase(os.path.realpath(p or os.curdir)) !=
               os.path.normcase(os.path.dirname(os.path.realpath(__file__)))]

import argparse
import datetime
import decimal
import http.client
import importlib.util
import io
import json
import math
import posixpath
import re
import shutil
import subprocess
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXCEPTIONS = ".gitlab/security-exceptions.yml"
VEX = ".gitlab/hmailserver.openvex.json"
BLOCKING = ("Critical", "High")


def _load(relative, name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, *relative.split("/")))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The exceptions file's reader and the change's base; OSV's export as Renovate's
# database takes it; and the native and vendored libraries as dependency-scan reads and
# asks OSV about them (which loads this file in turn, but only when it is asked to).
gate = _load("build/ci/security-gate.py", "security_gate")
osv_offline = _load("build/ci/osv-offline-db.py", "osv_offline_db")
scan = _load("build/ci/dependency-scan.py", "dependency_scan")


# ---------------------------------------------------------------- the lock files

def lock_kind(path):
    name = path.rsplit("/", 1)[-1]
    if re.fullmatch(r"packages(\.[A-Za-z0-9_-]+)*\.lock\.json", name):
        return "nuget-lock"
    if name == "packages.config":
        return "nuget-config"
    # Renovate's pip_requirements name rule ([\w-]*requirements([-.]\w+)?.(txt|pip)),
    # so every file Renovate updates is read here: build/ci/cli-packages-requirements.txt
    # as well as build/ci/requirements.txt and build/requirements-dev.txt.
    if re.fullmatch(r"[A-Za-z0-9_-]*requirements(?:[-.][A-Za-z0-9_]+)?\.(?:txt|pip)", name):
        return "pip"
    if re.fullmatch(r"native-dependencies[A-Za-z0-9_.-]*\.json", name):
        return "native"
    return None


def pypi_name(name):
    return re.sub(r"[-_.]+", "-", name).lower()


# What a requirements file may say beside its pins, name==version with the pin's hashes:
# pip's switches that only choose among a pinned version's own files, PyPI's own index,
# and a requirements or constraints file of the tree whose name is one Renovate's
# pip_requirements manager reads, which is read here on its own too. Any other line - an
# unpinned or ranged version, a URL or a path for a package, an editable, another index,
# a page of links, a requirements file pip would fetch or find outside the tree - is one
# pip would install from, and OSV cannot be asked about what it gives. A version or a
# file named through ${...}, which pip takes from the environment, pins nothing either.
PIP_SWITCHES = re.compile(r"^(?:--require-hashes|--prefer-binary|--no-deps|--(?:only|no)-binary(?:=|\s+)\S+)$")
PIP_INDEX = re.compile(r"^(?:-i|--index-url)(?:=|\s+)https://pypi\.org/simple/?$")
PIP_FILE = re.compile(r"^(?:-r|--requirement|-c|--constraint)(?:=|\s+)(\S+)$")
# A file -r or -c names that can be the tree's: a relative path, never a URL (or a
# drive), an absolute path or a home directory, and nothing pip expands.
PIP_LOCAL_FILE = re.compile(r"^(?![A-Za-z][A-Za-z0-9+.-]*:)(?![/~])[^$]+$")
PIP_PIN = re.compile(r"^([A-Za-z0-9][A-Za-z0-9._-]*)(\[[^\]]*\])?\s*===?\s*([^\s;*$]+)\s*(?:;.*)?$")


def pip_lines(text):
    """The logical lines of a requirements file, as pip reads them: a line that ends in a
    backslash goes on with the next, and a # at a line's start or after a blank begins a
    comment."""
    lines, held = [], ""
    for raw in text.lstrip("﻿").replace("\r\n", "\n").split("\n"):
        raw = re.sub(r"(^|\s)#.*$", "", raw).rstrip()
        if raw.endswith("\\"):
            held += raw[:-1] + " "
            continue
        lines.append((held + raw).strip())
        held = ""
    if held.strip():
        lines.append(held.strip())
    return [line for line in lines if line]


def parse_lock(kind, text, odd=None, included=None):
    """[(ecosystem, name as written, version)] of one lock file. ValueError when it
    cannot be read: a lock file this cannot read is never a clean one. odd, when given,
    collects each line of a requirements file that pins no version by name==version and
    that no other rule of PIP_SWITCHES, PIP_INDEX and PIP_FILE allows; included, when
    given, collects (the file named, the line) for each requirements or constraints file
    a line names, which the tree must hold (dependencies_at), or the line is odd."""
    found = []
    if kind == "nuget-lock":
        document = json.loads(text.lstrip("\ufeff"))
        for framework in (document.get("dependencies") or {}).values():
            for name, entry in (framework or {}).items():
                if not isinstance(entry, dict) or entry.get("type") == "Project":
                    continue
                if not entry.get("resolved"):
                    raise ValueError("%s has no resolved version" % name)
                found.append(("NuGet", name, entry["resolved"]))
    elif kind == "nuget-config":
        root = ET.fromstring(text.lstrip("\ufeff"))  # nosemgrep: a lock file of the tree being checked
        for package in root.iter("package"):
            if not package.get("id") or not package.get("version"):
                raise ValueError("a <package> without id or version")
            found.append(("NuGet", package.get("id"), package.get("version")))
    elif kind == "pip":
        for line in pip_lines(text):
            if line.startswith("-"):
                listed = PIP_FILE.match(line)
                named = listed.group(1).replace("\\", "/") if listed else ""
                if listed and lock_kind(named) == "pip" and PIP_LOCAL_FILE.match(named):
                    if included is not None:
                        included.append((named, line[:200]))
                elif not (PIP_SWITCHES.match(line) or PIP_INDEX.match(line)):
                    if odd is not None:
                        odd.append(line[:200])
                continue
            # A pin's hashes follow it; any other option on its line is pip's to act on.
            words = line.split()
            rest = " ".join(w for w in words if not w.startswith("--hash="))
            match = PIP_PIN.match(rest) if not re.search(r"\s-", " " + rest) else None
            if match:
                found.append(("PyPI", match.group(1), match.group(3)))
            elif odd is not None:
                odd.append(line[:200])
    elif kind == "native":
        document = json.loads(text.lstrip("\ufeff"))
        for component in document if isinstance(document, list) else []:
            if isinstance(component, dict) and component.get("name"):
                found.append(("native", component["name"], str(component.get("version") or "")))
    return found


def key_of(ecosystem, name, version):
    return (ecosystem, pypi_name(name) if ecosystem == "PyPI" else name.lower(), version.strip())


def git(root, *arguments):
    return subprocess.run(["git", "-C", root] + list(arguments), capture_output=True)


def dependencies_at(root, ref, odd=None, unreadable=None):
    """{key: [(file, name, version)]} of every lock file at ref. odd, when given, collects
    (file, line) for each requirements line that pins no version (parse_lock), and for
    each that names a requirements or constraints file ref does not hold, relative to the
    naming file's directory as pip reads it. A lock file that cannot be read is a
    ValueError, unless unreadable is given - at the base - when it collects (file, why)
    and the file is read as naming nothing."""
    listed = git(root, "ls-tree", "-r", "-z", "--name-only", ref)
    if listed.returncode != 0:
        raise ValueError("git ls-tree %s failed: %s" % (ref, listed.stderr.decode("utf-8", "replace").strip()[:300]))
    paths = [raw.decode("utf-8", "surrogateescape") for raw in listed.stdout.split(b"\0") if raw]
    tree = set(paths)
    found = {}
    for path in paths:
        kind = lock_kind(path)
        if not kind:
            continue
        shown = git(root, "show", "%s:%s" % (ref, path))
        if shown.returncode != 0:
            raise ValueError("git show %s:%s failed" % (ref, path))
        lines, included = [], []
        try:
            entries = parse_lock(kind, shown.stdout.decode("utf-8"), lines, included)
        except (ValueError, ET.ParseError, UnicodeDecodeError) as e:
            if unreadable is None:
                raise ValueError("%s at %s cannot be read: %s" % (path, ref[:12], e))
            unreadable.append((path, str(e)[:200]))
            continue
        # A file it names is read here only when it is one of the tree's.
        lines.extend(line for named, line in included
                     if posixpath.normpath(posixpath.join(posixpath.dirname(path), named)) not in tree)
        if odd is not None:
            odd.extend((path, line) for line in lines)
        for ecosystem, name, version in entries:
            found.setdefault(key_of(ecosystem, name, version), []).append((path, name, version))
    return found


def added_dependencies(root, base, odd_added=None, say=print):
    """[(key, [(file, name, version)])] at HEAD and not at base. odd_added, when given,
    collects (file, line) for each requirements line that pins no version and that the
    base does not have. A lock file the base has that cannot be read names nothing there,
    and is said: every version the head's names is then added, and no broken base holds
    every change, the one that mends it among them."""
    base_odd, head_odd, unreadable = [], [], []
    before = dependencies_at(root, base, base_odd, unreadable)
    after = dependencies_at(root, "HEAD", head_odd)
    for path, why in unreadable:
        say("The base's %s cannot be read (%s): it names nothing there, so every version this change's copy names is "
            "looked up." % (path, why))
    if odd_added is not None:
        odd_added.extend(sorted(set(head_odd) - set(base_odd)))
    return sorted((key, where) for key, where in after.items() if key not in before)


# ---------------------------------------------------------------- versions

_PEP440 = re.compile(
    r"^v?(?:(?P<epoch>[0-9]+)!)?(?P<release>[0-9]+(?:\.[0-9]+)*)"
    r"(?P<pre>[-_.]?(?P<pre_l>alpha|a|beta|b|preview|pre|c|rc)[-_.]?(?P<pre_n>[0-9]+)?)?"
    r"(?P<post>(?:-(?P<post_n1>[0-9]+))|(?:[-_.]?(?P<post_l>post|rev|r)[-_.]?(?P<post_n2>[0-9]+)?))?"
    r"(?P<dev>[-_.]?(?P<dev_l>dev)[-_.]?(?P<dev_n>[0-9]+)?)?"
    r"(?:\+(?P<local>[a-z0-9]+(?:[-_.][a-z0-9]+)*))?$", re.IGNORECASE)
_LAST = 1 << 62


def pep440_key(version):
    """PEP 440's order, as packaging's Version sorts; None for a version it does not parse."""
    m = _PEP440.match(version.strip())
    if not m:
        return None
    release = [int(p) for p in m.group("release").split(".")]
    while len(release) > 1 and release[-1] == 0:
        release.pop()
    pre_l = (m.group("pre_l") or "").lower()
    pre_l = {"alpha": "a", "beta": "b", "c": "rc", "pre": "rc", "preview": "rc"}.get(pre_l, pre_l)
    has_post = m.group("post") is not None
    has_dev = m.group("dev") is not None
    if not pre_l and not has_post and has_dev:
        pre = (-1, 0)
    elif not pre_l:
        pre = (3, 0)
    else:
        pre = ({"a": 0, "b": 1, "rc": 2}[pre_l], int(m.group("pre_n") or 0))
    post = -1 if not has_post else int(m.group("post_n1") or m.group("post_n2") or 0)
    dev = _LAST if not has_dev else int(m.group("dev_n") or 0)
    local = tuple((0, int(p)) if p.isdigit() else (-1, p.lower()) for p in re.split(r"[-_.]", m.group("local") or "") if p)
    return (int(m.group("epoch") or 0), tuple(release), pre, post, dev, local)


def semver_key(version):
    """NuGet's order (SemVer 2.0 with up to four numbers, case-insensitive labels); None
    for a version it does not parse."""
    m = re.match(r"^v?([0-9]+(?:\.[0-9]+){0,3})(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$", version.strip())
    if not m:
        return None
    numbers = [int(p) for p in m.group(1).split(".")]
    numbers += [0] * (4 - len(numbers))
    if m.group(2) is None:
        return (tuple(numbers), 1, ())
    labels = tuple((0, int(p), "") if p.isdigit() else (1, 0, p.lower()) for p in m.group(2).split("."))
    return (tuple(numbers), 0, labels)


def version_key(ecosystem, version):
    return pep440_key(version) if ecosystem == "PyPI" else semver_key(version)


def affects(ecosystem, affected, version):
    """Whether one affected[] entry of an advisory holds version: True, False, or None
    when this cannot tell - no "versions" entry names it, and it, or a version one of the
    entry's ranges is bounded by, is one this cannot order as the ecosystem does."""
    mine = version_key(ecosystem, version)
    for listed in affected.get("versions") or []:
        if listed == version or (mine is not None and version_key(ecosystem, listed) == mine):
            return True
    unsure = False
    for span in affected.get("ranges") or []:
        if span.get("type") not in ("ECOSYSTEM", "SEMVER"):
            continue
        if mine is None:
            unsure = True
            continue
        # OSV's evaluation: the events in version order, "introduced" opening the
        # range and "fixed" (at or below the version) or "last_affected" (below it)
        # closing it again; "introduced": "0" is before every version. A range with
        # a version this cannot order cannot be judged, which is said (None).
        events, readable = [], True
        for event in span.get("events") or []:
            for kind in ("introduced", "fixed", "last_affected"):
                if kind not in event:
                    continue
                at = str(event[kind])
                if kind == "introduced" and at == "0":
                    events.append(((0,), kind, None))
                    continue
                k = version_key(ecosystem, at)
                if k is None:
                    readable = False
                    continue
                events.append(((1, k), kind, k))
        if not readable:
            unsure = True
            continue
        inside = False
        for _, kind, k in sorted(events, key=lambda e: e[0]):
            if kind == "introduced" and (k is None or k <= mine):
                inside = True
            elif kind == "fixed" and k <= mine:
                inside = False
            elif kind == "last_affected" and k < mine:
                inside = False
        if inside:
            return True
    return None if unsure else False


# ---------------------------------------------------------------- severity

_CVSS3 = {
    "AV": {"N": 0.85, "A": 0.62, "L": 0.55, "P": 0.2},
    "AC": {"L": 0.77, "H": 0.44},
    "UI": {"N": 0.85, "R": 0.62},
    "C": {"H": 0.56, "L": 0.22, "N": 0.0},
}


def _roundup(value):
    whole = int(round(value * 100000))
    if whole % 10000 == 0:
        return whole / 100000.0
    return (math.floor(whole / 10000) + 1) / 10.0


def cvss3_score(vector):
    """The CVSS v3.0 or v3.1 base score of a vector, or None when it is not one."""
    parts = vector.split("/")
    if not parts or not re.match(r"^CVSS:3\.[01]$", parts[0]):
        return None
    metrics = dict(p.split(":", 1) for p in parts[1:] if ":" in p)
    try:
        scope = metrics["S"]
        av, ac, ui = _CVSS3["AV"][metrics["AV"]], _CVSS3["AC"][metrics["AC"]], _CVSS3["UI"][metrics["UI"]]
        pr = {"N": 0.85, "L": 0.68 if scope == "C" else 0.62, "H": 0.5 if scope == "C" else 0.27}[metrics["PR"]]
        c, i, a = (_CVSS3["C"][metrics[m]] for m in ("C", "I", "A"))
    except KeyError:
        return None
    if scope not in ("U", "C"):
        return None
    iss = 1 - (1 - c) * (1 - i) * (1 - a)
    impact = 6.42 * iss if scope == "U" else 7.52 * (iss - 0.029) - 3.25 * (iss - 0.02) ** 15
    exploitability = 8.22 * av * ac * pr * ui
    if impact <= 0:
        return 0.0
    if scope == "U":
        return _roundup(min(impact + exploitability, 10))
    return _roundup(min(1.08 * (impact + exploitability), 10))


# ---------------------------------------------------------------- CVSS v4.0

# FIRST's CVSS v4.0 score (the specification's section 8, and its calculator at
# https://www.first.org/cvss/calculator/4.0, whose tables these are): a vector falls in
# one macrovector of six equivalence classes, EQ1 to EQ6, whose score the table gives;
# the score is then lowered inside the class by how far the vector lies from the class's
# most severe vectors, as a share of the way to the next lower macrovector, each class
# in turn. Every base vector, with each Exploit Maturity, was compared with the
# calculator as Red Hat's cvss package 3.6 ports it, and none differs.
_CVSS4_MACROVECTORS = {
    "000000": 10, "000001": 9.9, "000010": 9.8, "000011": 9.5, "000020": 9.5, "000021": 9.2, "000100": 10, "000101": 9.6,
    "000110": 9.3, "000111": 8.7, "000120": 9.1, "000121": 8.1, "000200": 9.3, "000201": 9, "000210": 8.9, "000211": 8,
    "000220": 8.1, "000221": 6.8, "001000": 9.8, "001001": 9.5, "001010": 9.5, "001011": 9.2, "001020": 9, "001021": 8.4,
    "001100": 9.3, "001101": 9.2, "001110": 8.9, "001111": 8.1, "001120": 8.1, "001121": 6.5, "001200": 8.8, "001201": 8,
    "001210": 7.8, "001211": 7, "001220": 6.9, "001221": 4.8, "002001": 9.2, "002011": 8.2, "002021": 7.2, "002101": 7.9,
    "002111": 6.9, "002121": 5, "002201": 6.9, "002211": 5.5, "002221": 2.7, "010000": 9.9, "010001": 9.7, "010010": 9.5,
    "010011": 9.2, "010020": 9.2, "010021": 8.5, "010100": 9.5, "010101": 9.1, "010110": 9, "010111": 8.3, "010120": 8.4,
    "010121": 7.1, "010200": 9.2, "010201": 8.1, "010210": 8.2, "010211": 7.1, "010220": 7.2, "010221": 5.3, "011000": 9.5,
    "011001": 9.3, "011010": 9.2, "011011": 8.5, "011020": 8.5, "011021": 7.3, "011100": 9.2, "011101": 8.2, "011110": 8,
    "011111": 7.2, "011120": 7, "011121": 5.9, "011200": 8.4, "011201": 7, "011210": 7.1, "011211": 5.2, "011220": 5,
    "011221": 3, "012001": 8.6, "012011": 7.5, "012021": 5.2, "012101": 7.1, "012111": 5.2, "012121": 2.9, "012201": 6.3,
    "012211": 2.9, "012221": 1.7, "100000": 9.8, "100001": 9.5, "100010": 9.4, "100011": 8.7, "100020": 9.1, "100021": 8.1,
    "100100": 9.4, "100101": 8.9, "100110": 8.6, "100111": 7.4, "100120": 7.7, "100121": 6.4, "100200": 8.7, "100201": 7.5,
    "100210": 7.4, "100211": 6.3, "100220": 6.3, "100221": 4.9, "101000": 9.4, "101001": 8.9, "101010": 8.8, "101011": 7.7,
    "101020": 7.6, "101021": 6.7, "101100": 8.6, "101101": 7.6, "101110": 7.4, "101111": 5.8, "101120": 5.9, "101121": 5,
    "101200": 7.2, "101201": 5.7, "101210": 5.7, "101211": 5.2, "101220": 5.2, "101221": 2.5, "102001": 8.3, "102011": 7,
    "102021": 5.4, "102101": 6.5, "102111": 5.8, "102121": 2.6, "102201": 5.3, "102211": 2.1, "102221": 1.3, "110000": 9.5,
    "110001": 9, "110010": 8.8, "110011": 7.6, "110020": 7.6, "110021": 7, "110100": 9, "110101": 7.7, "110110": 7.5,
    "110111": 6.2, "110120": 6.1, "110121": 5.3, "110200": 7.7, "110201": 6.6, "110210": 6.8, "110211": 5.9, "110220": 5.2,
    "110221": 3, "111000": 8.9, "111001": 7.8, "111010": 7.6, "111011": 6.7, "111020": 6.2, "111021": 5.8, "111100": 7.4,
    "111101": 5.9, "111110": 5.7, "111111": 5.7, "111120": 4.7, "111121": 2.3, "111200": 6.1, "111201": 5.2, "111210": 5.7,
    "111211": 2.9, "111220": 2.4, "111221": 1.6, "112001": 7.1, "112011": 5.9, "112021": 3, "112101": 5.8, "112111": 2.6,
    "112121": 1.5, "112201": 2.3, "112211": 1.3, "112221": 0.6, "200000": 9.3, "200001": 8.7, "200010": 8.6, "200011": 7.2,
    "200020": 7.5, "200021": 5.8, "200100": 8.6, "200101": 7.4, "200110": 7.4, "200111": 6.1, "200120": 5.6, "200121": 3.4,
    "200200": 7, "200201": 5.4, "200210": 5.2, "200211": 4, "200220": 4, "200221": 2.2, "201000": 8.5, "201001": 7.5,
    "201010": 7.4, "201011": 5.5, "201020": 6.2, "201021": 5.1, "201100": 7.2, "201101": 5.7, "201110": 5.5, "201111": 4.1,
    "201120": 4.6, "201121": 1.9, "201200": 5.3, "201201": 3.6, "201210": 3.4, "201211": 1.9, "201220": 1.9, "201221": 0.8,
    "202001": 6.4, "202011": 5.1, "202021": 2, "202101": 4.7, "202111": 2.1, "202121": 1.1, "202201": 2.4, "202211": 0.9,
    "202221": 0.4, "210000": 8.8, "210001": 7.5, "210010": 7.3, "210011": 5.3, "210020": 6, "210021": 5, "210100": 7.3,
    "210101": 5.5, "210110": 5.9, "210111": 4, "210120": 4.1, "210121": 2, "210200": 5.4, "210201": 4.3, "210210": 4.5,
    "210211": 2.2, "210220": 2, "210221": 1.1, "211000": 7.5, "211001": 5.5, "211010": 5.8, "211011": 4.5, "211020": 4,
    "211021": 2.1, "211100": 6.1, "211101": 5.1, "211110": 4.8, "211111": 1.8, "211120": 2, "211121": 0.9, "211200": 4.6,
    "211201": 1.8, "211210": 1.7, "211211": 0.7, "211220": 0.8, "211221": 0.2, "212001": 5.3, "212011": 2.4, "212021": 1.4,
    "212101": 2.4, "212111": 1.2, "212121": 0.5, "212201": 1, "212211": 0.3, "212221": 0.1,
}
# The most severe vectors of each class, and each class's depth in steps of 0.1.
_CVSS4_HIGHEST = {
    "eq1": {0: ["AV:N/PR:N/UI:N"], 1: ["AV:A/PR:N/UI:N", "AV:N/PR:L/UI:N", "AV:N/PR:N/UI:P"], 2: ["AV:P/PR:N/UI:N", "AV:A/PR:L/UI:P"]},
    "eq2": {0: ["AC:L/AT:N"], 1: ["AC:H/AT:N", "AC:L/AT:P"]},
    "eq3eq6": {(0, 0): ["VC:H/VI:H/VA:H/CR:H/IR:H/AR:H"],
               (0, 1): ["VC:H/VI:H/VA:L/CR:M/IR:M/AR:H", "VC:H/VI:H/VA:H/CR:M/IR:M/AR:M"],
               (1, 0): ["VC:L/VI:H/VA:H/CR:H/IR:H/AR:H", "VC:H/VI:L/VA:H/CR:H/IR:H/AR:H"],
               (1, 1): ["VC:L/VI:H/VA:L/CR:H/IR:M/AR:H", "VC:L/VI:H/VA:H/CR:H/IR:M/AR:M", "VC:H/VI:L/VA:H/CR:M/IR:H/AR:M",
                        "VC:H/VI:L/VA:L/CR:M/IR:H/AR:H", "VC:L/VI:L/VA:H/CR:H/IR:H/AR:M"],
               (2, 1): ["VC:L/VI:L/VA:L/CR:H/IR:H/AR:H"]},
    "eq4": {0: ["SC:H/SI:S/SA:S"], 1: ["SC:H/SI:H/SA:H"], 2: ["SC:L/SI:L/SA:L"]},
    "eq5": {0: ["E:A"], 1: ["E:P"], 2: ["E:U"]},
}
_CVSS4_DEPTH = {"eq1": {0: 1, 1: 4, 2: 5}, "eq2": {0: 1, 1: 2},
                "eq3eq6": {(0, 0): 7, (0, 1): 6, (1, 0): 8, (1, 1): 8, (2, 1): 10}, "eq4": {0: 6, 1: 5, 2: 4}}
_CVSS4_LEVELS = {
    "AV": {"N": 0.0, "A": 0.1, "L": 0.2, "P": 0.3}, "PR": {"N": 0.0, "L": 0.1, "H": 0.2}, "UI": {"N": 0.0, "P": 0.1, "A": 0.2},
    "AC": {"L": 0.0, "H": 0.1}, "AT": {"N": 0.0, "P": 0.1},
    "VC": {"H": 0.0, "L": 0.1, "N": 0.2}, "VI": {"H": 0.0, "L": 0.1, "N": 0.2}, "VA": {"H": 0.0, "L": 0.1, "N": 0.2},
    "SC": {"H": 0.1, "L": 0.2, "N": 0.3}, "SI": {"S": 0.0, "H": 0.1, "L": 0.2, "N": 0.3}, "SA": {"S": 0.0, "H": 0.1, "L": 0.2, "N": 0.3},
    "CR": {"H": 0.0, "M": 0.1, "L": 0.2}, "IR": {"H": 0.0, "M": 0.1, "L": 0.2}, "AR": {"H": 0.0, "M": 0.1, "L": 0.2},
}
# Every metric a v4.0 vector may carry, with its values: the eleven base metrics, which
# it must carry, then the threat, environmental and supplemental ones.
_CVSS4_BASE = ("AV", "AC", "AT", "PR", "UI", "VC", "VI", "VA", "SC", "SI", "SA")
_CVSS4_VALUES = {
    "AV": "NALP", "AC": "LH", "AT": "NP", "PR": "NLH", "UI": "NPA", "VC": "HLN", "VI": "HLN", "VA": "HLN",
    "SC": "HLN", "SI": "HLN", "SA": "HLN", "E": "XAPU", "CR": "XHML", "IR": "XHML", "AR": "XHML",
    "MAV": "XNALP", "MAC": "XLH", "MAT": "XNP", "MPR": "XNLH", "MUI": "XNPA", "MVC": "XHLN", "MVI": "XHLN",
    "MVA": "XHLN", "MSC": "XHLN", "MSI": "XSHLN", "MSA": "XSHLN", "S": "XNP", "AU": "XNY", "R": "XAUI",
    "V": "XDC", "RE": "XLMH", "U": ["X", "Clear", "Green", "Amber", "Red"],
}


def cvss4_score(vector):
    """The CVSS v4.0 score of a vector (CVSS-B, or CVSS-BT and so on as its metrics go),
    or None when it is not a v4.0 vector with every base metric and known values."""
    parts = vector.strip().split("/")
    if parts[0] != "CVSS:4.0":
        return None
    given = {}
    for part in parts[1:]:
        name, _, value = part.partition(":")
        # A whole value of the metric's own: "in" over the string of letters would also
        # take "" or "AP" (E:AP), which no table holds.
        if name not in _CVSS4_VALUES or name in given or value not in list(_CVSS4_VALUES[name]):
            return None
        given[name] = value
    if any(name not in given for name in _CVSS4_BASE):
        return None

    def m(name):
        # The value the score uses: a modified metric's where it is set; the threat and
        # the requirements, when not set, as the specification's worst case.
        value = given.get(name, "X")
        if name == "E" and value == "X":
            return "A"
        if name in ("CR", "IR", "AR") and value == "X":
            return "H"
        modified = given.get("M" + name, "X")
        return modified if modified != "X" else value

    if all(m(name) == "N" for name in ("VC", "VI", "VA", "SC", "SI", "SA")):
        return 0.0
    av, pr, ui = m("AV"), m("PR"), m("UI")
    if av == "N" and pr == "N" and ui == "N":
        eq1 = 0
    elif (av == "N" or pr == "N" or ui == "N") and av != "P":
        eq1 = 1
    else:
        eq1 = 2
    eq2 = 0 if m("AC") == "L" and m("AT") == "N" else 1
    if m("VC") == "H" and m("VI") == "H":
        eq3 = 0
    elif m("VC") == "H" or m("VI") == "H" or m("VA") == "H":
        eq3 = 1
    else:
        eq3 = 2
    if given.get("MSI") == "S" or given.get("MSA") == "S":
        eq4 = 0
    elif m("SC") == "H" or m("SI") == "H" or m("SA") == "H":
        eq4 = 1
    else:
        eq4 = 2
    eq5 = {"A": 0, "P": 1, "U": 2}[m("E")]
    eq6 = 0 if ((m("CR") == "H" and m("VC") == "H") or (m("IR") == "H" and m("VI") == "H")
                or (m("AR") == "H" and m("VA") == "H")) else 1

    def score_of(*eqs):
        return _CVSS4_MACROVECTORS.get("".join(str(e) for e in eqs), float("nan"))

    value = score_of(eq1, eq2, eq3, eq4, eq5, eq6)
    lower_eq1 = score_of(eq1 + 1, eq2, eq3, eq4, eq5, eq6)
    lower_eq2 = score_of(eq1, eq2 + 1, eq3, eq4, eq5, eq6)
    if eq3 == 0 and eq6 == 0:
        lower_eq3eq6 = max(score_of(eq1, eq2, eq3, eq4, eq5, eq6 + 1), score_of(eq1, eq2, eq3 + 1, eq4, eq5, eq6))
    elif eq3 == 1 and eq6 == 0:
        lower_eq3eq6 = score_of(eq1, eq2, eq3, eq4, eq5, eq6 + 1)
    elif eq6 == 1 and eq3 in (0, 1):
        lower_eq3eq6 = score_of(eq1, eq2, eq3 + 1, eq4, eq5, eq6)
    else:
        lower_eq3eq6 = score_of(eq1, eq2, eq3 + 1, eq4, eq5, eq6 + 1)
    lower_eq4 = score_of(eq1, eq2, eq3, eq4 + 1, eq5, eq6)
    lower_eq5 = score_of(eq1, eq2, eq3, eq4, eq5 + 1, eq6)

    # The distance of each metric from the first of the class's most severe vectors that
    # is at least as severe as the vector in every metric (the last one, if none is).
    distance = {}
    for eq1_max in _CVSS4_HIGHEST["eq1"][eq1]:
        for eq2_max in _CVSS4_HIGHEST["eq2"][eq2]:
            for eq3eq6_max in _CVSS4_HIGHEST["eq3eq6"][(eq3, eq6)]:
                for eq4_max in _CVSS4_HIGHEST["eq4"][eq4]:
                    for eq5_max in _CVSS4_HIGHEST["eq5"][eq5]:
                        highest = dict(p.split(":") for p in "/".join((eq1_max, eq2_max, eq3eq6_max, eq4_max, eq5_max)).split("/"))
                        distance = dict((name, _CVSS4_LEVELS[name][m(name)] - _CVSS4_LEVELS[name][highest[name]])
                                        for name in _CVSS4_LEVELS)
                        if all(d >= 0 for d in distance.values()):
                            break
                    else:
                        continue
                    break
                else:
                    continue
                break
            else:
                continue
            break
        else:
            continue
        break
    current_eq1 = distance["AV"] + distance["PR"] + distance["UI"]
    current_eq2 = distance["AC"] + distance["AT"]
    current_eq3eq6 = distance["VC"] + distance["VI"] + distance["VA"] + distance["CR"] + distance["IR"] + distance["AR"]
    current_eq4 = distance["SC"] + distance["SI"] + distance["SA"]
    step = 0.1
    parts_of_mean, existing = [], 0
    for available, current, depth in ((value - lower_eq1, current_eq1, _CVSS4_DEPTH["eq1"][eq1] * step),
                                      (value - lower_eq2, current_eq2, _CVSS4_DEPTH["eq2"][eq2] * step),
                                      (value - lower_eq3eq6, current_eq3eq6, _CVSS4_DEPTH["eq3eq6"][(eq3, eq6)] * step),
                                      (value - lower_eq4, current_eq4, _CVSS4_DEPTH["eq4"][eq4] * step),
                                      (value - lower_eq5, 0, None)):
        if available >= 0:
            existing += 1
            parts_of_mean.append(0 if depth is None else available * (current / depth))
        else:
            parts_of_mean.append(0)
    mean = 0 if existing == 0 else (parts_of_mean[0] + parts_of_mean[1] + parts_of_mean[2] + parts_of_mean[3]
                                    + parts_of_mean[4]) / existing
    value = min(10.0, max(0.0, value - mean))
    # One decimal, half away from zero, past float noise (8.6 - 7.15 is 1.4499999999999993).
    return float(decimal.Decimal(value + 1e-6).quantize(decimal.Decimal("0.1"), rounding=decimal.ROUND_HALF_UP))


def rating_of_score(score):
    if score >= 9.0:
        return "Critical"
    if score >= 7.0:
        return "High"
    if score >= 4.0:
        return "Medium"
    if score > 0:
        return "Low"
    return None


RANK = {"Critical": 4, "High": 3, "Medium": 2, "Low": 1}


def own_rating(advisory):
    """(rating, where it came from) of one advisory alone, or (None, None)."""
    if str(advisory.get("id", "")).startswith("MAL-"):
        return "Critical", "a malicious package (OpenSSF malicious-packages)"
    label = str((advisory.get("database_specific") or {}).get("severity") or "").upper()
    mapped = {"CRITICAL": "Critical", "HIGH": "High", "MODERATE": "Medium", "MEDIUM": "Medium", "LOW": "Low"}.get(label)
    if mapped:
        return mapped, "its database's rating, %s" % label
    for kind, score_of in (("CVSS_V3", cvss3_score), ("CVSS_V4", cvss4_score)):
        for entry in advisory.get("severity") or []:
            if entry.get("type") == kind:
                score = score_of(str(entry.get("score") or ""))
                if score is not None and rating_of_score(score):
                    return rating_of_score(score), "%s %.1f" % ("CVSS" if kind == "CVSS_V3" else "CVSS v4.0", score)
    return None, None


def rating(advisory, index):
    """(rating, where it came from): its own, or else the highest of its aliases'."""
    mine = own_rating(advisory)
    if mine[0]:
        return mine
    best = (None, None)
    for alias in advisory.get("aliases") or []:
        other = index.get(alias)
        if other is None:
            continue
        theirs = own_rating(other)
        if theirs[0] and (best[0] is None or RANK[theirs[0]] > RANK[best[0]]):
            best = (theirs[0], "its alias %s: %s" % (alias, theirs[1]))
    return best


# ---------------------------------------------------------------- the VEX document
#
# .gitlab/hmailserver.openvex.json, OpenVEX: a statement dismisses an advisory for the
# products it names when its status is not_affected (or fixed). It is matched here by
# the advisory's id or one of its aliases AND by the package at its version, by purl:
# pkg:nuget/<name>@<version> (NuGet's names without case), pkg:pypi/<name>@<version>
# (PEP 503's names), pkg:generic/<name>@<version> (the native and vendored libraries,
# by the purl build/native-dependencies.ps1 gives each). A product with no version
# dismisses nothing: a statement names the version it was read against. The purl's
# qualifiers and subpath are not compared. build/ci/dependency-scan.py reads the
# document with these too.

def parse_purl(purl):
    """(type, name with its namespace, version) of a purl, percent-decoded, the type in
    lower case; None for what is not one."""
    m = re.match(r"^pkg:([A-Za-z][A-Za-z0-9.+-]*)/([^@?#]+)(?:@([^?#]*))?(?:\?[^#]*)?(?:#.*)?$", (purl or "").strip())
    if not m:
        return None
    return m.group(1).lower(), urllib.parse.unquote(m.group(2)).strip("/"), urllib.parse.unquote(m.group(3) or "")


def vex_statements(text):
    """[(the advisory's names, [(type, name, version)] of its products and their
    subcomponents, why)] of every not_affected or fixed statement. ValueError when the
    text is not an OpenVEX document."""
    document = json.loads(text.lstrip("﻿"))
    if not isinstance(document, dict) or not isinstance(document.get("statements"), list):
        raise ValueError("not an OpenVEX document: no list of statements")
    found = []
    for statement in document["statements"]:
        if not isinstance(statement, dict) or statement.get("status") not in ("not_affected", "fixed"):
            continue
        vulnerability = statement.get("vulnerability") or {}
        if not isinstance(vulnerability, dict):
            continue
        names = set(str(n) for n in [vulnerability.get("name")] + list(vulnerability.get("aliases") or []) if n)
        products = []
        for product in statement.get("products") or []:
            entries = [product]
            if isinstance(product, dict):
                entries += list(product.get("subcomponents") or [])
            for entry in entries:
                ids = [entry] if isinstance(entry, str) else (
                    [entry.get("@id"), (entry.get("identifiers") or {}).get("purl")] if isinstance(entry, dict) else [])
                for value in ids:
                    parsed = parse_purl(value) if isinstance(value, str) else None
                    if parsed and parsed not in products:
                        products.append(parsed)
        why = statement.get("justification") or statement.get("impact_statement") or statement["status"]
        found.append((names, products, why))
    return found


def vex_dismissal(statements, ids, kind, name, version):
    """Why the VEX document's statements dismiss the advisory known by ids for the
    package kind/name (purl type and name) at version, or None when none does."""
    ecosystem = {"nuget": "NuGet", "pypi": "PyPI"}.get(kind)
    wanted = set(i for i in ids if i)
    for names, products, why in statements:
        if not names & wanted:
            continue
        for product_kind, product_name, product_version in products:
            if product_kind != kind or not product_version:
                continue
            if kind == "pypi":
                same_name = pypi_name(product_name) == pypi_name(name)
            else:
                same_name = product_name.lower() == name.lower()
            if not same_name:
                continue
            if product_version == version or (ecosystem and version_key(ecosystem, version) is not None
                                              and version_key(ecosystem, product_version) == version_key(ecosystem, version)):
                return why
    return None


# ---------------------------------------------------------------- the advisories

def load_ecosystem(ecosystem, osv_dir=None):
    """Every advisory of an ecosystem in OSV's export, {id: advisory}."""
    if osv_dir:
        with open(os.path.join(osv_dir, "%s.zip" % ecosystem), "rb") as f:
            data = f.read()
    else:
        data = osv_offline.fetch(ecosystem)
    archive = zipfile.ZipFile(io.BytesIO(data))
    advisories = {}
    for member in archive.namelist():
        if member.endswith(".json"):
            advisory = json.loads(archive.read(member))
            if advisory.get("id"):
                advisories[advisory["id"]] = advisory
    if not advisories:
        raise ValueError("OSV's %s export holds no advisory" % ecosystem)
    return advisories


def advisories_for(ecosystem, name, version, index, unsure=None):
    """The advisories that affect one version of one package, not withdrawn; those that
    may (affects() cannot tell) are added to unsure, when it is given."""
    wanted = key_of(ecosystem, name, version)[1]
    hits = []
    for advisory in index.values():
        if advisory.get("withdrawn"):
            continue
        maybe = False
        for affected in advisory.get("affected") or []:
            package = affected.get("package") or {}
            if package.get("ecosystem") != ecosystem or key_of(ecosystem, package.get("name") or "", "")[1] != wanted:
                continue
            verdict = affects(ecosystem, affected, version)
            if verdict:
                hits.append(advisory)
                maybe = False
                break
            if verdict is None:
                maybe = True
        if maybe and unsure is not None:
            unsure.append(advisory)
    return hits


def by_name(index, ecosystem):
    """{normalized package name: [advisory]} - so each added package reads only its own."""
    names = {}
    for advisory in index.values():
        for affected in advisory.get("affected") or []:
            package = affected.get("package") or {}
            if package.get("ecosystem") == ecosystem and package.get("name"):
                names.setdefault(key_of(ecosystem, package["name"], "")[1], {})[advisory["id"]] = advisory
    return names


# ---------------------------------------------------------------- the native and vendored libraries

class OsvApi(object):
    """api.osv.dev, as build/ci/dependency-scan.py asks it: by a repository's tag (GIT),
    and for one record by its id. An error is raised, never taken for no advisory."""

    def query(self, repo, tag):
        return scan.osv_query(repo, tag)

    def vuln(self, advisory_id):
        """The record of one id, or None when OSV has none (404)."""
        request = urllib.request.Request("%s/vulns/%s" % (scan.OSV_API, urllib.parse.quote(advisory_id, safe="")),
                                         headers={"User-Agent": "hmailserver-mr-dependency-check"})
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.load(response)
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise


def native_pins(root, ref, at_head):
    """{library name: [the versions ref pins]} for every library of dependency-scan's
    VENDORED, read from its file as that scan reads it. At the head a file that is not
    there, or names no version this can read, is a ValueError: a pin that cannot be read
    is never a clean one. At the base it pins nothing, so every version at the head is
    asked about."""
    pins, texts = {}, {}
    for lib in scan.VENDORED:
        path = lib["file"]
        if path not in texts:
            shown = git(root, "show", "%s:%s" % (ref, path))
            if shown.returncode == 0:
                texts[path] = shown.stdout.decode("utf-8", "replace")
            elif git(root, "cat-file", "-e", "%s:%s" % (ref, path)).returncode == 0:
                raise ValueError("git show %s:%s failed: %s" % (ref[:12], path, shown.stderr.decode("utf-8", "replace").strip()[:200]))
            else:
                texts[path] = None
        text = texts[path]
        if text is None:
            if at_head:
                raise ValueError("%s is not in the tree, and %s's version is read from it (build/ci/dependency-scan.py's "
                                 "VENDORED)" % (path, lib["name"]))
            pins[lib["name"]] = []
            continue
        try:
            versions = scan.vendored_versions(lib, text)
        except ValueError as e:
            if at_head:
                raise ValueError("%s cannot be read for %s's version: %s" % (path, lib["name"], e))
            versions = []
        if at_head and not versions:
            raise ValueError("%s names no version of %s that build/ci/dependency-scan.py reads (%s)" % (
                path, lib["name"], lib.get("component") or lib.get("pattern")))
        pins[lib["name"]] = versions
    return pins


def native_changes(root, base, native_added):
    """[(library of VENDORED or None, name, version, files)]: each version of a library
    that HEAD pins and base does not, and each component a committed native list adds,
    taken as the library of VENDORED whose purl name it has (None when it is none)."""
    before = native_pins(root, base, False)
    after = native_pins(root, "HEAD", True)
    changes, seen = [], set()
    for lib in scan.VENDORED:
        for version in after[lib["name"]]:
            if version not in before[lib["name"]]:
                changes.append((lib, lib["name"], version, lib["file"]))
                seen.add((lib["name"], version))
    by_purl = dict((lib["purl"], lib) for lib in scan.VENDORED)
    for key, where in native_added:
        _, name, version = key
        lib = by_purl.get(name.lower())
        files = ", ".join(sorted({w[0] for w in where}))
        if lib is not None and (lib["name"], version) in seen:
            continue
        if lib is not None:
            seen.add((lib["name"], version))
        changes.append((lib, lib["name"] if lib else where[0][1], version, files))
    return changes


def looked_up_rating(advisory, osv):
    """(rating, where it came from): its own, or else the highest of its aliases'
    records as api.osv.dev gives them."""
    mine = own_rating(advisory)
    if mine[0]:
        return mine
    best = (None, None)
    for alias in advisory.get("aliases") or []:
        other = osv.vuln(alias)
        theirs = own_rating(other) if other else (None, None)
        if theirs[0] and (best[0] is None or RANK[theirs[0]] > RANK[best[0]]):
            best = (theirs[0], "its alias %s: %s" % (alias, theirs[1]))
    return best


def evaluate_native(changes, accepted, statements, osv, say, held_versions):
    """The problems a native or vendored library's changed version holds the change on."""
    problems = []
    for lib, name, version, files in changes:
        what = "%s %s (%s)" % (name, version, files)
        if lib is None:
            say("UNKNOWN %s: a native component that is no library of build/ci/dependency-scan.py's VENDORED, so no "
                "upstream repository is known to ask OSV by; listed, not held" % what)
            continue
        if not lib.get("repo"):
            say("UNKNOWN %s: %s; listed, not held" % (what, lib.get("coverage") or "OSV has no source for it"))
            continue

        def tag_of(v):
            return lib["tag"].format(v=v, v_=v.replace(".", "_"))
        tag, control = tag_of(version), tag_of(lib["control"])
        found = osv.query(lib["repo"], tag)
        control_found = osv.query(lib["repo"], control)
        if lib.get("control_draws") and not control_found:
            # The control is one OSV is known to name: its silence is OSV's data not read,
            # never an answer.
            raise ValueError("api.osv.dev names no advisory for %s's control %s, which it is known to name (dependency-scan's "
                             "VENDORED): its answer for %s cannot be taken" % (lib["name"], control, tag))
        maybe, below = [], None
        if lib.get("oldest_tag") and scan.version_key(version) < scan.version_key(lib["oldest_tag"]):
            # OSV matches a version by its tag, and the repository has none this old: what
            # affects its first tag most probably reaches back to it.
            below = lib["oldest_tag"]
            maybe = [v for v in osv.query(lib["repo"], tag_of(lib["oldest_tag"]))
                     if v.get("id") not in {f.get("id") for f in found}]
            say("        %s is older than %s's first tag, %s, which OSV cannot match: the advisories of %s are taken as "
                "ones that may affect it (%d)" % (version, lib["repo"], lib["oldest_tag"], lib["oldest_tag"], len(maybe)))
        elif not found and control_found and scan.version_key(version) < scan.version_key(lib["control"]):
            # Older than the control, which OSV names advisories for, and named in none
            # itself: OSV did not match the tag (OpenSSL's 0.9.6 draws none where 4.0.0
            # draws 43), and what affects the control most probably reaches back to it.
            below = lib["control"]
            maybe = list(control_found)
            say("        %s is older than the control %s, and OSV names no advisory for it where it names %d for the "
                "control: the control's are taken as ones that may affect it" % (version, lib["control"], len(maybe)))
        if not found and not maybe:
            if not control_found:
                say("UNKNOWN %s: OSV names no advisory for %s, but none for the control %s either, which a published "
                    "advisory affects: OSV has no usable data for this library%s; listed, not held" % (
                        what, tag, control, (" (%s)" % lib["coverage"]) if lib.get("coverage") else ""))
            else:
                say("ok      native %s: no advisory names %s (the control %s draws %d)" % (what, tag, control, len(control_found)))
            continue
        sure_ids = {v.get("id") for v in found}
        for group in alias_groups(found + maybe):
            uncertain = not any(a.get("id") in sure_ids for a in group)
            rated = [(looked_up_rating(a, osv), a) for a in group]
            (level, source), advisory = max(rated, key=lambda r: (RANK.get(r[0][0], 0), r[1]["id"].startswith("CVE-")))
            ids = sorted({advisory["id"]} | {i for a in group for i in [a["id"]] + list(a.get("aliases") or [])})
            others = [i for i in ids if i != advisory["id"]]
            taken = next((accepted[(i, pypi_name(lib["name"]))] for i in ids if (i, pypi_name(lib["name"])) in accepted), None)
            fixes = sorted({str(ev["fixed"])[:12] for a in advisory.get("affected") or [] for r in a.get("ranges") or []
                            for ev in r.get("events") or [] if "fixed" in ev})
            line = "native %s: %s %s (%s)%s%s - %s" % (
                what, level or "unrated", advisory["id"], ", ".join(others) or "no alias",
                (" - fix commit %s" % ", ".join(fixes[:4])) if fixes else " - no fix",
                (" - MAY affect it: OSV cannot match %s, and this advisory affects %s" % (version, below))
                if uncertain else "",
                (advisory.get("summary") or advisory.get("details") or "")[:140].replace("\n", " "))
            if level in BLOCKING and not taken:
                why = vex_dismissal(statements, ids, "generic", lib["purl"], version)
                if why:
                    say("DISMISSED by the base's VEX document (%s): %s" % (why, line))
                    continue
                problems.append(line + " [rated by %s]" % source)
                held_versions.add(("native", lib["name"], version))
            elif taken:
                say("ACCEPTED until %s (%s): %s" % (taken["expires"], taken["reviewed-by"], line))
            else:
                say("listed  %s%s" % (line, "" if level else " [no rating in OSV's record or its aliases': not held]"))
    return problems


# ---------------------------------------------------------------- the verdict

PURL_TYPES = {"NuGet": "nuget", "PyPI": "pypi"}


def evaluate(added, exceptions_text, today, loader, say=print, held_versions=None, statements=None, native=None, osv=None):
    """The problems that hold the change, one per vulnerability of an added version;
    held_versions, when given, collects the versions they hold. loader(ecosystem) gives
    {id: advisory}; statements are the base's VEX document's (vex_statements); native
    is native_changes', asked of osv (OsvApi's query and vuln)."""
    held_versions = set() if held_versions is None else held_versions
    statements = statements or []
    native = native or []
    problems = []
    # A base's exceptions file that cannot be read, or an entry of it reviewed after
    # today, accepts nothing, and is said, not held: held, it would hold every change,
    # the one that mends it among them.
    entries, wrong = gate.check_exceptions(exceptions_text)
    if wrong:
        say("The base's %s cannot be read, so it accepts nothing here: %s" % (EXCEPTIONS, "; ".join(wrong)))
        entries = []
    in_force, wrong = gate._in_force(entries, today, say)
    for line in wrong:
        say("Accepts nothing here: %s" % line)
    accepted = {}
    for e in in_force.get("dependency-scan", []):
        accepted[(e["finding"], pypi_name(e["package"]))] = e

    if not added and not native:
        say("No dependency version is added, and no native or vendored library's pin moves: nothing to look up.")
        return problems
    if native:
        problems.extend(evaluate_native(native, accepted, statements, osv or OsvApi(), say, held_versions))
    else:
        say("No native or vendored library's pin moves: api.osv.dev is not asked.")
    loaded, indexes = {}, {}
    for key, where in added:
        ecosystem, name, version = key
        files = ", ".join(sorted({w[0] for w in where}))
        shown = where[0][1]
        if ecosystem not in PURL_TYPES:
            # A native list's component: main_check gives those to native_changes.
            raise ValueError("%s %s (%s) is of no ecosystem OSV's export has" % (shown, version, files))
        if ecosystem not in loaded:
            loaded[ecosystem] = loader(ecosystem)
            indexes[ecosystem] = by_name(loaded[ecosystem], ecosystem)
            say("OSV's %s export: %d advisories" % (ecosystem, len(loaded[ecosystem])))
        candidates = indexes[ecosystem].get(name, {})
        unsure = []
        hits = advisories_for(ecosystem, shown, version, candidates, unsure)
        if not hits and not unsure:
            say("ok      %s %s %s (%s): no advisory" % (ecosystem, shown, version, files))
            continue
        sure_ids = {a["id"] for a in hits}
        for group in alias_groups(hits + unsure):
            # A vulnerability one of whose records affects the version for certain is
            # certain; one this can only say may affect it is said so.
            maybe = not any(a["id"] in sure_ids for a in group)
            # One vulnerability under several ids (a GHSA and its PYSEC, say) is said
            # once, by the record that rates it highest, with the others as aliases.
            rated = [(rating(a, loaded[ecosystem]), a) for a in group]
            (level, source), advisory = max(rated, key=lambda r: (RANK.get(r[0][0], 0), r[1]["id"].startswith("GHSA-")))
            ids = sorted({advisory["id"]} | {i for a in group for i in [a["id"]] + list(a.get("aliases") or [])})
            others = [i for i in ids if i != advisory["id"]]
            # The package as the exception names it, compared as PyPI compares names
            # (case, and runs of - _ . alike), which also leaves NuGet's case aside.
            taken = next((accepted[(i, pypi_name(shown))] for i in ids if (i, pypi_name(shown)) in accepted), None)
            fixed = sorted({str(ev["fixed"]) for a in advisory.get("affected") or [] for r in a.get("ranges") or []
                            for ev in r.get("events") or [] if "fixed" in ev})
            line = "%s %s %s (%s): %s %s (%s)%s%s - %s" % (
                ecosystem, shown, version, files, level or "unrated", advisory["id"], ", ".join(others) or "no alias",
                (" - fixed in %s" % ", ".join(fixed[:4])) if fixed else " - no fixed version",
                (" - MAY affect it: this cannot order %s, or a version bounding the advisory's range, as %s does, and no "
                 "version list names it" % (version, ecosystem)) if maybe else "",
                (advisory.get("summary") or advisory.get("details") or "")[:140].replace("\n", " "))
            if level in BLOCKING and not taken:
                why = vex_dismissal(statements, ids, PURL_TYPES[ecosystem], shown, version)
                if why:
                    say("DISMISSED by the base's VEX document (%s): %s" % (why, line))
                    continue
                problems.append(line + " [rated by %s]" % source)
                held_versions.add(key)
            elif taken:
                say("ACCEPTED until %s (%s): %s" % (taken["expires"], taken["reviewed-by"], line))
            else:
                say("listed  %s%s" % (line, "" if level else " [no rating in OSV's record or its aliases': not held]"))
    return problems


def alias_groups(advisories):
    """The advisories, grouped where one names another as an alias (transitively)."""
    groups = []
    for advisory in sorted(advisories, key=lambda a: a["id"]):
        names = {advisory["id"]} | set(advisory.get("aliases") or [])
        joined = [g for g in groups if g[0] & names]
        merged_names, members = set(names), [advisory]
        for g in joined:
            merged_names |= g[0]
            members = g[1] + members
            groups.remove(g)
        groups.append((merged_names, members))
    return [members for _, members in groups]


def base_statements(root, base, say):
    """The base's VEX document's statements. One the base does not have dismisses
    nothing; one it has that cannot be read dismisses nothing either, and is said: what
    it would have dismissed then holds the change, never the other way round."""
    shown = git(root, "show", "%s:%s" % (base, VEX))
    if shown.returncode != 0:
        if git(root, "cat-file", "-e", "%s:%s" % (base, VEX)).returncode == 0:
            raise ValueError("git show %s:%s failed" % (base[:12], VEX))
        return []
    try:
        return vex_statements(shown.stdout.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as e:
        say("The base's %s cannot be read (%s): it dismisses nothing here." % (VEX, e))
        return []


def main_check(root, base, osv_dir=None, today=None, say=print, osv=None):
    if not re.match(r"^[0-9a-f]{7,64}$", base or ""):
        say("CANNOT CHECK: no base commit (%r): give --base auto, or the merge request's CI_MERGE_REQUEST_DIFF_BASE_SHA" % base)
        return 2
    if git(root, "cat-file", "-e", base + "^{commit}").returncode != 0:
        say("CANNOT CHECK: the base commit %s is not in this clone: the job needs the history (GIT_DEPTH 0)" % base)
        return 2
    odd = []
    try:
        added = added_dependencies(root, base, odd, say)
        # A committed native component list's entries are native libraries' pins.
        native = native_changes(root, base, [a for a in added if a[0][0] not in PURL_TYPES])
        added = [a for a in added if a[0][0] in PURL_TYPES]
        statements = base_statements(root, base, say)
    except ValueError as e:
        say("CANNOT CHECK: %s" % e)
        return 2
    shown = git(root, "show", "%s:%s" % (base, EXCEPTIONS))
    exceptions_text = shown.stdout.decode("utf-8") if shown.returncode == 0 else "exceptions: []\n"
    today = today or datetime.datetime.now(datetime.timezone.utc).date()
    held_versions = set()
    try:
        problems = evaluate(added, exceptions_text, today, lambda e: load_ecosystem(e, osv_dir), say, held_versions,
                            statements, native, osv)
    except (OSError, ValueError, zipfile.BadZipFile, http.client.HTTPException) as e:
        say("CANNOT CHECK: OSV's advisories could not be read (%s): no answer is not a clean answer; retry the job" % e)
        return 2
    unpinned = ["%s: %r pins no version by name==version, or names an index, a link, a requirements file the tree does "
                "not hold or an option of pip's beside PyPI's own: pip would install what OSV cannot be asked about. Pin "
                "it by name==version with its hashes, as Renovate's pip_requirements manager does" % (path, line)
                for path, line in odd]
    problems = unpinned + problems
    for p in problems:
        say("HELD: " + p)
    if problems:
        advisories = sum(1 for p in problems if " [rated by " in p)
        say("This change is held: %d added dependency version(s) with %d Critical or High advisory(ies) between "
            "them%s. Take a version no such advisory affects, or have the advisory declared not exploitable - accepted "
            "for the package in %s, or not_affected for it at that version in %s - by a merge request of its own, "
            "landed first (SECURITY.md, 'Vulnerability management policy')." % (
                len(held_versions), advisories,
                (", and %d requirement line(s) that pin no version" % len(unpinned)) if unpinned else "", EXCEPTIONS, VEX))
        return 1
    say("No added dependency version has a Critical or High advisory that the base's %s or %s does not accept."
        % (EXCEPTIONS, VEX))
    return 0


# ---------------------------------------------------------------- the self-test

def self_test():
    failures = []
    quiet = lambda line: None

    def expect(condition, what):
        if not condition:
            failures.append(what)

    # The orders.
    order = ["1.0.dev1", "1.0a1", "1.0a2.dev3", "1.0b1", "1.0rc1", "1.0", "1.0.post1", "1.0.1", "1!0.1"]
    keys = [pep440_key(v) for v in order]
    expect(None not in keys and keys == sorted(keys), "PEP 440 orders %s" % order)
    expect(pep440_key("1.0") == pep440_key("1.0.0") == pep440_key("v1.0"), "PEP 440: 1.0, 1.0.0 and v1.0 are one version")
    expect(pep440_key("not a version") is None, "PEP 440 refuses what it cannot read")
    order = ["1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-RC.1", "1.0.0",
             "1.0.0.1", "1.0.1", "10.0.0"]
    keys = [semver_key(v) for v in order]
    expect(None not in keys and keys == sorted(keys), "NuGet orders %s" % order)
    expect(semver_key("1.0") == semver_key("1.0.0.0"), "NuGet: 1.0 and 1.0.0.0 are one version")
    # FIRST's own examples of the v3.1 calculator.
    for vector, score in [("CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H", 9.8),
                          ("CVSS:3.1/AV:N/AC:L/PR:N/UI:R/S:C/C:L/I:L/A:N", 6.1),
                          ("CVSS:3.1/AV:L/AC:L/PR:L/UI:N/S:U/C:H/I:H/A:H", 7.8),
                          ("CVSS:3.0/AV:N/AC:H/PR:N/UI:N/S:U/C:H/I:N/A:N", 5.9),
                          ("CVSS:3.1/AV:N/AC:L/PR:L/UI:N/S:C/C:H/I:H/A:H", 9.9),
                          ("CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:N/I:N/A:N", 0.0)]:
        got = cvss3_score(vector)
        expect(got == score, "CVSS %s is %s, not %s" % (vector, score, got))
    expect(cvss3_score("CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N") is None,
           "a CVSS v4 vector is not read as v3")
    # CVSS v4.0: FIRST's calculator's answers (and the cvss package's), base, threat and
    # environmental metrics among them, and vectors that are not v4.0 ones.
    for vector, score in [("CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N", 9.3),
                          ("CVSS:4.0/AV:L/AC:L/AT:N/PR:L/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N", 8.5),
                          ("CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:P/VC:L/VI:L/VA:N/SC:N/SI:N/SA:N", 5.3),
                          ("CVSS:4.0/AV:N/AC:H/AT:P/PR:L/UI:A/VC:L/VI:N/VA:N/SC:N/SI:N/SA:N", 2.0),
                          ("CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:H/SI:H/SA:H", 10.0),
                          ("CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N/E:U", 8.1),
                          ("CVSS:4.0/AV:P/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N/CR:L/IR:L/AR:L/MSI:S", 8.3),
                          ("CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:N/VI:N/VA:N/SC:N/SI:N/SA:N", 0.0)]:
        got = cvss4_score(vector)
        expect(got == score, "CVSS %s is %s, not %s" % (vector, score, got))
    for vector in ("CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N",
                   "CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N/SA:H",
                   "CVSS:4.0/AV:Q/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N",
                   "CVSS:4.0/AV:/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N",
                   "CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:HL/VI:H/VA:H/SC:N/SI:N/SA:N",
                   "CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N/E:AP",
                   "CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N/U:Cle",
                   "CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H"):
        expect(cvss4_score(vector) is None, "%s is not read as a v4.0 vector" % vector)

    def advisory(id, ecosystem, name, versions=None, ranges=None, severity=None, cvss=None, aliases=(), withdrawn=None,
                 cvss4=None):
        record = {"id": id, "summary": "%s in %s" % (id, name), "aliases": list(aliases),
                  "affected": [{"package": {"ecosystem": ecosystem, "name": name}, "versions": versions or [],
                                "ranges": ranges or []}]}
        if severity:
            record["database_specific"] = {"severity": severity}
        if cvss:
            record["severity"] = [{"type": "CVSS_V3", "score": cvss}]
        if cvss4:
            record["severity"] = [{"type": "CVSS_V4", "score": cvss4}]
        if withdrawn:
            record["withdrawn"] = withdrawn
        return record

    def span(*events):
        return [{"type": "ECOSYSTEM", "events": [dict([e]) for e in events]}]

    nuget = [advisory("GHSA-aaaa-aaaa-aaaa", "NuGet", "Foo.Bar", ranges=span(("introduced", "0"), ("fixed", "2.0.0")), severity="HIGH"),
             advisory("GHSA-bbbb-bbbb-bbbb", "NuGet", "Foo.Bar", ranges=span(("introduced", "3.0.0"), ("last_affected", "3.1.0")),
                      severity="MODERATE"),
             advisory("GHSA-wwww-wwww-wwww", "NuGet", "Foo.Bar", versions=["4.0.0"], severity="CRITICAL", withdrawn="2026-01-01T00:00:00Z"),
             advisory("GHSA-vvvv-vvvv-vvvv", "NuGet", "Vector.Only", ranges=span(("introduced", "1.0"), ("fixed", "1.2")),
                      cvss="CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H"),
             # Rated by a CVSS v4.0 vector alone, 8.5: High.
             advisory("GHSA-4444-4444-4444", "NuGet", "Four.Only", ranges=span(("introduced", "0"), ("fixed", "2.0.0")),
                      cvss4="CVSS:4.0/AV:L/AC:L/AT:N/PR:L/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N"),
             # A range bounded by a version NuGet's order cannot read.
             advisory("GHSA-uuuu-uuuu-uuuu", "NuGet", "Odd.Range", ranges=span(("introduced", "0"), ("fixed", "2.0.0_final")),
                      severity="HIGH")]
    pypi = [advisory("PYSEC-2026-1", "PyPI", "requests", versions=["2.0.0"], aliases=["GHSA-cccc-cccc-cccc"]),
            advisory("GHSA-cccc-cccc-cccc", "PyPI", "Requests", ranges=span(("introduced", "2.0"), ("fixed", "2.0.1")), severity="CRITICAL"),
            advisory("MAL-2026-7", "PyPI", "evil_pkg", ranges=span(("introduced", "0"))),
            advisory("PYSEC-2026-2", "PyPI", "quiet", versions=["1.0"])]

    def archive(records):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as z:
            for r in records:
                z.writestr("%s.json" % r["id"], json.dumps(r))
        return buffer.getvalue()

    work = tempfile.mkdtemp(prefix="mr-dependency-check-")
    try:
        osv_dir = os.path.join(work, "osv")
        os.makedirs(osv_dir)
        for name, records in (("NuGet", nuget), ("PyPI", pypi)):
            with open(os.path.join(osv_dir, "%s.zip" % name), "wb") as f:
                f.write(archive(records))
        index = load_ecosystem("NuGet", osv_dir)
        expect([a["id"] for a in advisories_for("NuGet", "foo.bar", "1.5.0", index)] == ["GHSA-aaaa-aaaa-aaaa"],
               "a version inside introduced 0 .. fixed 2.0.0 is affected, the name compared without case")
        expect(advisories_for("NuGet", "Foo.Bar", "2.0.0", index) == [], "the fixed version is not")
        expect([a["id"] for a in advisories_for("NuGet", "Foo.Bar", "3.1.0", index)] == ["GHSA-bbbb-bbbb-bbbb"], "last_affected is")
        expect(advisories_for("NuGet", "Foo.Bar", "3.1.1", index) == [], "the version after last_affected is not")
        expect(advisories_for("NuGet", "Foo.Bar", "4.0.0", index) == [], "a withdrawn advisory affects nothing")
        pindex = load_ecosystem("PyPI", osv_dir)
        expect(rating(pindex["PYSEC-2026-1"], pindex)[0] == "Critical", "an unrated advisory takes its alias's rating")
        expect(rating(index["GHSA-vvvv-vvvv-vvvv"], index)[0] == "Critical", "a CVSS vector alone is rated by its score")
        expect(rating(pindex["MAL-2026-7"], pindex)[0] == "Critical", "a malicious package is Critical")
        expect(rating(index["GHSA-4444-4444-4444"], index)[0] == "High", "a CVSS v4.0 vector alone is rated by its score")
        unsure = []
        expect(advisories_for("NuGet", "Odd.Range", "1.0.0", index, unsure) == [] and
               [a["id"] for a in unsure] == ["GHSA-uuuu-uuuu-uuuu"],
               "a range bounded by a version that cannot be ordered is neither a hit nor a pass, but undecided")
        unsure = []
        expect(advisories_for("NuGet", "Foo.Bar", "1.0.0.0.0", index, unsure) == [] and
               sorted(a["id"] for a in unsure) == ["GHSA-aaaa-aaaa-aaaa", "GHSA-bbbb-bbbb-bbbb"],
               "a version that cannot be ordered against an advisory with a range is undecided")

        repo = os.path.join(work, "repo")
        os.makedirs(repo)
        env = ["-c", "core.autocrlf=false", "-c", "user.name=self-test", "-c", "user.email=self-test@example.invalid",
               "-c", "commit.gpgsign=false"]
        subprocess.run(["git", "init", "-q", repo], check=True, capture_output=True)

        def write(files):
            for relative, text in files.items():
                path = os.path.join(repo, *relative.split("/"))
                if text is None:
                    os.remove(path)
                    continue
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)

        def commit(files, message):
            write(files)
            subprocess.run(["git", "-C", repo] + env + ["add", "-A"], check=True, capture_output=True)
            subprocess.run(["git", "-C", repo] + env + ["commit", "-q", "--no-verify", "--allow-empty", "-m", message],
                           check=True, capture_output=True)
            return subprocess.run(["git", "-C", repo, "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()

        def lock(**packages):
            return json.dumps({"version": 1, "dependencies": {"net10.0": dict(
                [(n.replace("_", "."), {"type": "Direct", "requested": "[%s, )" % v, "resolved": v, "contentHash": "x"})
                 for n, v in packages.items()] + [("Shared", {"type": "Project"})])}}, indent=2)

        def requirements(**packages):
            return "# pinned\n" + "".join("%s==%s \\\n    --hash=sha256:%s\n" % (n.replace("_", "-"), v, "0" * 64)
                                          for n, v in packages.items())

        # The native and vendored libraries' files, as the tree pins them, by the paths
        # dependency-scan's VENDORED reads: changed below, one version at a time.
        libs = dict((lib["purl"], lib) for lib in scan.VENDORED)

        def natives(openssl="4.0.3", openssl_arm64=None, boost="1_92_0", zlib="1.3.2", quickjs="2026-06-04",
                    mariadb="3.4.9", sevenzip="26.03", sqlce="4.0 SP1 (MSI built 2012-04-06)"):
            project = ("<I>$(hMailServerLibs)\\boost_%s;$(hMailServerLibs)\\openssl-%s\\out64\\include;"
                       "$(hMailServerLibs)\\postgresql-18.6\\src\\interfaces\\libpq</I>\n"
                       "<I>$(hMailServerLibs)\\openssl-%s-arm64\\out64\\include</I>\n" % (boost, openssl, openssl_arm64 or openssl))
            binaries = json.dumps({"artifacts": [
                {"component": "MariaDB Connector/C", "version": mariadb, "path": "libmysql.dll"},
                {"component": "MariaDB Connector/C", "version": mariadb, "path": "plugin/dialog.dll"},
                {"component": "7-Zip standalone console (7za.exe)", "version": sevenzip},
                {"component": "Microsoft SQL Server Compact 4.0 SP1 x64 ENU", "version": sqlce}]}, indent=2)
            return {libs["openssl"]["file"]: project, libs["zlib"]["file"]: '#define ZLIB_VERSION "%s"\n' % zlib,
                    libs["quickjs"]["file"]: quickjs + "\n", libs["7-zip"]["file"]: binaries}

        def vex(*statements):
            return json.dumps({"@context": "https://openvex.dev/ns/v0.2.0", "statements": [
                {"vulnerability": {"name": name, "aliases": list(aliases)}, "products": [{"@id": p} for p in products],
                 "status": "not_affected", "justification": "vulnerable_code_not_present"}
                for name, aliases, products in statements]}, indent=2)

        empty = "exceptions: []\n"
        files = {"tools/A/packages.lock.json": lock(Foo_Bar="2.0.0"), "build/requirements.txt": requirements(requests="2.0.1"),
                 EXCEPTIONS: empty}
        files.update(natives())
        base = commit(files, "base")

        # api.osv.dev, as OsvApi asks it: by a repository's tag, and one record by id.
        def native_record(id, vector=None, aliases=(), summary=None):
            record = {"id": id, "aliases": list(aliases), "summary": summary or "%s in a native library" % id,
                      "affected": [{"ranges": [{"type": "GIT", "repo": "r", "events": [{"introduced": "0"},
                                                                                        {"fixed": "f" * 40}]}]}]}
            if vector:
                record["severity"] = [{"type": "CVSS_V4" if vector.startswith("CVSS:4.0/") else "CVSS_V3", "score": vector}]
            return record

        critical = "CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H"
        local_high = "CVSS:3.1/AV:L/AC:L/PR:L/UI:N/S:U/C:H/I:H/A:H"
        medium = "CVSS:3.1/AV:N/AC:L/PR:N/UI:R/S:C/C:L/I:L/A:N"

        def answer(purl, tag, *records):
            return ((libs[purl]["repo"], tag), list(records))
        answers = dict([
            answer("zlib", "v1.2.11", native_record("CVE-2022-37434", critical, summary="inflateGetHeader heap overflow")),
            answer("zlib", "v1.3", native_record("CVE-2026-27171", medium)),
            answer("openssl", "openssl-4.0.0", native_record("CVE-2026-1", medium)),
            answer("boost", "boost-1.93.0", native_record("OSV-2026-9", aliases=["CVE-2026-9999"])),
            answer("boost", "boost-1.90.0", native_record("OSV-2026-8")),
            answer("mariadb-connector-c", "v3.4.10", native_record("CVE-2026-4", medium)),
            answer("mariadb-connector-c", "v3.1.7", native_record("CVE-2020-1", critical)),
            answer("7-zip", "22.01", native_record("CVE-2023-31102", local_high)),
            answer("7-zip", "24.08", native_record("CVE-2024-11477", local_high))])

        class FakeOsv(object):
            def __init__(self, broken=False):
                self.broken, self.asked = broken, []

            def query(self, repo, tag):
                self.asked.append((repo, tag))
                if self.broken:
                    raise OSError("api.osv.dev cannot be reached")
                return [dict(r) for r in answers.get((repo, tag), [])]

            def vuln(self, advisory_id):
                self.asked.append(("vuln", advisory_id))
                return {"CVE-2026-9999": native_record("CVE-2026-9999", local_high)}.get(advisory_id)

        said = []
        fake = [FakeOsv()]

        def verdict(base_commit, loader_dir=osv_dir, osv=None):
            del said[:]
            fake[0] = osv or FakeOsv()
            return main_check(repo, base_commit, loader_dir, datetime.date(2026, 10, 1), say=said.append, osv=fake[0])

        def reset(to):
            subprocess.run(["git", "-C", repo, "reset", "-q", "--hard", to], check=True, capture_output=True)

        commit({"tools/B/packages.lock.json": lock(Foo_Bar="1.5.0")}, "a second project on an older version")
        expect(verdict(base) == 1 and any("GHSA-aaaa-aaaa-aaaa" in s for s in said if s.startswith("HELD")),
               "an added NuGet version with a High advisory is held: %s" % said)
        reset(base)
        commit({"tools/A/packages.lock.json": lock(Foo_Bar="2.1.0")}, "an upgrade past the fix")
        expect(verdict(base) == 0, "an upgrade to a version no advisory names passes: %s" % said)
        reset(base)
        commit({"tools/A/packages.lock.json": lock(Foo_Bar="3.0.5")}, "a version with a Moderate advisory")
        expect(verdict(base) == 0 and any("Medium GHSA-bbbb" in s for s in said), "a Moderate advisory is listed, not held: %s" % said)
        reset(base)
        commit({"build/requirements.txt": requirements(requests="2.0.0")}, "a PyPI version with a Critical alias")
        code = verdict(base)
        held = [s for s in said if s.startswith("HELD")]
        expect(code == 1 and len(held) == 1 and "GHSA-cccc-cccc-cccc" in held[0] and "PYSEC-2026-1" in held[0],
               "an added PyPI version whose advisory's alias is Critical is held, once for the one vulnerability, "
               "by the record that rates it: %s" % said)
        reset(base)
        commit({"build/requirements-extra.txt": requirements(evil_pkg="0.1")}, "a malicious package")
        expect(verdict(base) == 1 and any("MAL-2026-7" in s for s in said if s.startswith("HELD")), "a malicious package is held: %s" % said)
        reset(base)
        commit({"tools/C/packages.lock.json": lock(Four_Only="1.0.0")}, "a version rated High by CVSS v4.0 alone")
        expect(verdict(base) == 1 and any("GHSA-4444-4444-4444" in s and "CVSS v4.0 8.5" in s for s in said
                                          if s.startswith("HELD")),
               "an advisory rated High by its CVSS v4.0 vector alone holds the merge request: %s" % said)
        reset(base)
        commit({"tools/C/packages.lock.json": lock(Odd_Range="1.0.0")}, "a version an advisory's range cannot be judged for")
        expect(verdict(base) == 1 and any("GHSA-uuuu-uuuu-uuuu" in s and "MAY affect" in s for s in said
                                          if s.startswith("HELD")),
               "a High advisory that may affect the version, which this cannot tell, holds the merge request: %s" % said)
        reset(base)
        commit({"build/requirements-extra.txt": requirements(quiet="1.0")}, "an unrated advisory")
        expect(verdict(base) == 0 and any("no rating" in s for s in said), "an advisory with no rating anywhere is listed: %s" % said)
        reset(base)
        commit({"docs/readme.txt": "nothing\n"}, "no lock file")
        expect(verdict(base, os.path.join(work, "nowhere")) == 0 and any("nothing to look up" in s for s in said),
               "a merge request that adds no dependency passes without reading OSV: %s" % said)
        reset(base)
        commit({"tools/A/packages.lock.json": "{ broken"}, "an unreadable lock file")
        expect(verdict(base) == 2, "a lock file that cannot be read is never a pass")
        reset(base)
        unresolved = json.dumps({"version": 1, "dependencies": {"net10.0": {"Foo.Bar": {"type": "Direct", "requested": "[1.0, )"}}}})
        commit({"tools/A/packages.lock.json": unresolved}, "a package the lock file does not resolve")
        expect(verdict(base) == 2, "a package with no resolved version is never a pass")
        reset(base)
        commit({"tools/B/packages.lock.json": lock(Foo_Bar="1.5.0")}, "an older version, again")
        expect(verdict(base, os.path.join(work, "nowhere")) == 2, "advisories that cannot be read are never a pass")
        reset(base)
        accepting = "\n".join(["exceptions:", "  - scan: dependency-scan", "    finding: GHSA-aaaa-aaaa-aaaa",
                               "    package: Foo.Bar", "    what: 'Foo.Bar 1.5.0 in the B tool'",
                               "    reason: 'Read: the B tool never parses the input the advisory needs; it is a build step.'",
                               "    reviewed-by: Progressiverobot", "    reviewed: 2026-09-28", "    expires: 2027-03-28", ""])
        commit({"tools/B/packages.lock.json": lock(Foo_Bar="1.5.0"), EXCEPTIONS: accepting}, "accepting its own advisory")
        expect(verdict(base) == 1, "a merge request that accepts its own advisory in its own copy is still held: %s" % said)
        reset(base)
        accepted_base = commit({EXCEPTIONS: accepting}, "the exception, landed first")
        commit({"tools/B/packages.lock.json": lock(Foo_Bar="1.5.0")}, "the change after it")
        expect(verdict(accepted_base) == 0 and any(s.startswith("ACCEPTED") for s in said),
               "an advisory the base accepts for the package passes: %s" % said)
        reset(base)
        commit({"docs/readme.txt": "nothing\n"}, "nothing native moves")
        expect(verdict(base) == 0 and fake[0].asked == [], "a change that moves no native pin does not ask api.osv.dev: %s"
               % fake[0].asked)

        # The native and vendored libraries.
        reset(base)
        commit(natives(zlib="1.2.11"), "zlib back to 1.2.11")
        expect(verdict(base) == 1 and any("native zlib 1.2.11" in s and "CVE-2022-37434" in s for s in said
                                          if s.startswith("HELD")),
               "a vendored zlib moved to a version with a Critical advisory is held (fixture 3): %s" % said)
        expect(("https://github.com/madler/zlib", "v1.2.11") in fake[0].asked and ("https://github.com/madler/zlib", "v1.3")
               in fake[0].asked, "OSV is asked by zlib's tag, and its control beside it: %s" % fake[0].asked)
        reset(base)
        commit(natives(zlib="1.2.11"), "zlib back to 1.2.11")
        commit({VEX: vex(("CVE-2022-37434", (), ["pkg:generic/zlib@1.2.11"]))}, "its own declaration")
        expect(verdict(base) == 1, "a change whose own VEX document dismisses its advisory is still held: %s" % said)
        reset(base)
        declared = commit({VEX: vex(("CVE-2022-37434", (), ["pkg:generic/zlib@1.2.11"]))}, "the declaration, landed first")
        commit(natives(zlib="1.2.11"), "zlib back to 1.2.11")
        expect(verdict(declared) == 0 and any(s.startswith("DISMISSED") and "CVE-2022-37434" in s for s in said),
               "an advisory the base's VEX document says zlib 1.2.11 is not affected by passes: %s" % said)
        reset(base)
        other = commit({VEX: vex(("CVE-2022-37434", (), ["pkg:generic/zlib@1.2.12"]))}, "a declaration for another version")
        commit(natives(zlib="1.2.11"), "zlib back to 1.2.11")
        expect(verdict(other) == 1, "a VEX statement for zlib 1.2.12 does not dismiss the advisory for 1.2.11: %s" % said)
        reset(base)
        zlib_exception = accepting.replace("GHSA-aaaa-aaaa-aaaa", "CVE-2022-37434").replace("package: Foo.Bar", "package: zlib")
        excepted = commit({EXCEPTIONS: zlib_exception}, "an exception for zlib, landed first")
        commit(natives(zlib="1.2.11"), "zlib back to 1.2.11")
        expect(verdict(excepted) == 0 and any(s.startswith("ACCEPTED") and "CVE-2022-37434" in s for s in said),
               "an advisory the base's exceptions accept for the library by its dependency-scan name passes: %s" % said)
        reset(base)
        commit(natives(openssl="4.0.3", openssl_arm64="4.0.4"), "OpenSSL 4.0.4 for ARM64 alone")
        expect(verdict(base) == 0 and any(s.startswith("ok      native OpenSSL 4.0.4") for s in said),
               "a version a project file names once among others is asked about, and passes with no advisory: %s" % said)
        reset(base)
        commit(natives(quickjs="2026-09-01"), "a QuickJS OSV knows nothing of")
        expect(verdict(base) == 0 and any(s.startswith("UNKNOWN QuickJS 2026-09-01") for s in said),
               "a library OSV has no data for, not even for its control, is UNKNOWN: listed, not held: %s" % said)
        reset(base)
        commit(natives(sqlce="4.1"), "another SQL Server Compact")
        expect(verdict(base) == 0 and any(s.startswith("UNKNOWN SQL Server Compact 4.0 4.1") for s in said),
               "a library with no repository OSV knows is UNKNOWN: listed, not held: %s" % said)
        reset(base)
        commit(natives(sevenzip="21.07"), "a 7-Zip older than its repository's first tag")
        expect(verdict(base) == 1 and any("CVE-2023-31102" in s and "MAY affect" in s for s in said if s.startswith("HELD")),
               "a version older than the first tag is held by that tag's High advisories, as ones that may affect it: %s"
               % said)
        reset(base)
        commit(natives(boost="1_93_0"), "a Boost whose advisory is rated by its alias")
        expect(verdict(base) == 1 and any("OSV-2026-9" in s and "its alias CVE-2026-9999" in s for s in said
                                          if s.startswith("HELD")),
               "an unrated native advisory takes its alias's rating, read from api.osv.dev: %s" % said)
        reset(base)
        commit(natives(mariadb="3.4.10"), "a Connector/C with a Medium advisory")
        expect(verdict(base) == 0 and any(s.startswith("listed  native MariaDB Connector/C 3.4.10") for s in said),
               "a Medium native advisory is listed, not held: %s" % said)
        reset(base)
        commit(natives(zlib="1.2.11"), "zlib back to 1.2.11")
        expect(verdict(base, osv=FakeOsv(broken=True)) == 2, "api.osv.dev that cannot be asked is never a pass: %s" % said)
        reset(base)
        commit({libs["zlib"]["file"]: None}, "zlib.h gone")
        expect(verdict(base) == 2, "a pinned library's file that is not there is never a pass: %s" % said)
        reset(base)
        commit({libs["zlib"]["file"]: "/* no version */\n"}, "zlib.h without its version")
        expect(verdict(base) == 2, "a pin that cannot be read is never a pass: %s" % said)
        reset(base)
        commit({"tools/A/native-dependencies-arm64.json": json.dumps([{"name": "zlib", "version": "1.2.11"},
                                                                      {"name": "libfoo", "version": "1.0"}])},
               "a native component list")
        expect(verdict(base) == 1 and any("native zlib 1.2.11" in s for s in said if s.startswith("HELD"))
               and any(s.startswith("UNKNOWN libfoo 1.0") for s in said),
               "a native list's component is asked about as the library of VENDORED with its purl name, and one that is "
               "none is listed: %s" % said)
        # A control OSV is known to name that draws nothing is OSV's data not read; a
        # version older than a control that draws advisories, named in none itself, is
        # one OSV did not match, held by the control's.
        reset(base)
        commit(natives(zlib="1.3.1"), "zlib 1.3.1, OSV's control answering nothing")
        silent = FakeOsv()
        silent.query = lambda repo, tag: [] if tag == "v1.3" else FakeOsv.query(silent, repo, tag)
        expect(verdict(base, osv=silent) == 2 and any("known to name" in s for s in said),
               "a control OSV is known to name that draws nothing is never a pass: %s" % said)
        reset(base)
        commit(natives(mariadb="3.0.0"), "a Connector/C older than its control, which OSV names nothing for")
        expect(verdict(base) == 1 and any("CVE-2020-1" in s and "MAY affect" in s for s in said if s.startswith("HELD")),
               "a version below a control that draws a Critical advisory, named in none itself, is held by it: %s" % said)

        # A requirements line that pins no version by name==version is held, and one the
        # base already has is not the change's. A requirements file it names is the tree's
        # (build/requirements-dev.txt, committed with it), never one pip would fetch or
        # find elsewhere; and nothing pip takes from the environment pins anything.
        for line, holds in (("requests>=2.0", True), ("requests @ https://example.invalid/requests-2.0.whl --hash=sha256:%s"
                                                       % ("0" * 64), True),
                            ("--extra-index-url https://example.invalid/simple", True), ("-e git+https://x.invalid/r#egg=r", True),
                            ("--index-url https://pypi.org/simple", False), ("-r requirements-dev.txt", False),
                            ("-r ../build/requirements-dev.txt", False), ("--requirement=requirements-dev.txt", False),
                            ("-r requirements-missing.txt", True), ("-r ../../requirements-dev.txt", True),
                            ("-r https://example.invalid/requirements.txt", True),
                            ("--requirement=http://example.invalid/requirements-dev.txt", True),
                            ("-r /etc/requirements.txt", True), ("-r C:/requirements.txt", True),
                            ("-r ${HOME}/requirements.txt", True), ("requests==${REQUESTS_VERSION}", True),
                            ("-r other.in", True), ("--require-hashes", False), ("requests==2.*", True),
                            ("requests==2.0.1 ; python_version >= '3.9' \\\n    --hash=sha256:%s" % ("0" * 64), False),
                            ("requests==2.0.1 --global-option=x", True)):
            reset(base)
            commit({"build/requirements.txt": requirements(requests="2.0.1") + line + "\n",
                    "build/requirements-dev.txt": requirements(requests="2.0.1")}, "a requirement line")
            code = verdict(base)
            expect((code == 1) == holds and (any("pins no version" in s for s in said) == holds),
                   "%r %s: exit %d, %s" % (line, "holds the change" if holds else "is read", code, said))
        reset(base)
        loose = commit({"build/requirements.txt": requirements(requests="2.0.1") + "requests>=2.0\n"}, "a loose line, landed")
        commit({"docs/readme.txt": "nothing\n"}, "a change after it")
        expect(verdict(loose) == 0, "a requirements line the base already has does not hold a change: %s" % said)

        # A broken base mends: its unreadable lock file reads as none, and its exceptions
        # file that cannot be read accepts nothing; neither holds the change by itself.
        reset(base)
        broken_lock = commit({"tools/A/packages.lock.json": "{ broken"}, "a base whose lock file cannot be read")
        commit({"tools/A/packages.lock.json": lock(Foo_Bar="2.0.0")}, "the lock file mended")
        expect(verdict(broken_lock) == 0 and any("cannot be read" in s and "names nothing there" in s for s in said),
               "a base lock file that cannot be read names nothing, and the change that mends it passes: %s" % said)
        reset(base)
        broken_exceptions = commit({EXCEPTIONS: "exceptions:\n  - scan: dependency-scan\n"}, "a base exceptions file broken")
        commit({EXCEPTIONS: empty}, "the exceptions file mended")
        expect(verdict(broken_exceptions) == 0 and any("cannot be read, so it accepts nothing" in s for s in said),
               "a base exceptions file that cannot be read accepts nothing, and holds no change by itself: %s" % said)
        commit({"tools/B/packages.lock.json": lock(Foo_Bar="1.5.0")}, "and an advisory with it")
        expect(verdict(broken_exceptions) == 1, "what that file would have accepted holds: %s" % said)
        reset(base)
        future = accepting.replace("reviewed: 2026-09-28", "reviewed: 2026-12-01").replace("expires: 2027-03-28", "expires: 2027-06-01")
        early = commit({EXCEPTIONS: future}, "an exception reviewed after today")
        commit({"docs/readme.txt": "nothing\n"}, "a change after it")
        expect(verdict(early) == 0 and any("after today" in s for s in said),
               "an entry of the base's reviewed after today accepts nothing and holds no change by itself: %s" % said)

        # The base's VEX document for the package ecosystems: pkg:nuget and pkg:pypi.
        reset(base)
        dismissing = commit({VEX: vex(("GHSA-aaaa-aaaa-aaaa", (), ["pkg:nuget/foo.bar@1.5.0"]))}, "a NuGet declaration")
        commit({"tools/B/packages.lock.json": lock(Foo_Bar="1.5.0")}, "the version it declares")
        expect(verdict(dismissing) == 0 and any(s.startswith("DISMISSED") for s in said),
               "an added NuGet version the base's VEX document dismisses by purl passes, the name without case: %s" % said)
        reset(base)
        elsewhere = commit({VEX: vex(("GHSA-aaaa-aaaa-aaaa", (), ["pkg:nuget/Foo.Bar@1.4.0"]))}, "a declaration for 1.4.0")
        commit({"tools/B/packages.lock.json": lock(Foo_Bar="1.5.0")}, "1.5.0")
        expect(verdict(elsewhere) == 1, "a NuGet declaration for another version dismisses nothing: %s" % said)
        reset(base)
        python = commit({VEX: vex(("CVE-2026-0404", ("GHSA-cccc-cccc-cccc",), ["pkg:pypi/Requests@2.0"]))},
                        "a PyPI declaration by an alias")
        commit({"build/requirements.txt": requirements(requests="2.0.0")}, "requests 2.0.0")
        expect(verdict(python) == 0 and any(s.startswith("DISMISSED") for s in said),
               "a PyPI declaration matches by alias, PEP 503's name and PEP 440's version: %s" % said)
        reset(base)
        broken_vex = commit({VEX: "{ not json"}, "a VEX document that cannot be read")
        commit({"build/requirements.txt": requirements(requests="2.0.0")}, "requests 2.0.0")
        expect(verdict(broken_vex) == 1 and any("cannot be read" in s for s in said),
               "a base VEX document that cannot be read dismisses nothing, and says so: %s" % said)
        statements = vex_statements(json.dumps({"statements": [
            {"vulnerability": {"name": "CVE-1"}, "status": "not_affected",
             "products": [{"@id": "pkg:generic/hmailserver@6.3.6", "subcomponents": [{"@id": "pkg:generic/zlib@1.3.2?arch=x64"}]}]},
            {"vulnerability": {"name": "CVE-2"}, "status": "under_investigation", "products": ["pkg:generic/zlib@1.3.2"]}]}))
        expect(vex_dismissal(statements, ["CVE-1"], "generic", "zlib", "1.3.2"),
               "a product's subcomponent is matched, its purl's qualifiers aside")
        expect(vex_dismissal(statements, ["CVE-2"], "generic", "zlib", "1.3.2") is None, "under_investigation dismisses nothing")
        expect(parse_purl("pkg:nuget/Foo%2EBar@1.0") == ("nuget", "Foo.Bar", "1.0") and parse_purl("zlib@1.3.2") is None,
               "a purl is read with its percent-escapes, and what is not one is not")

        expect(verdict("") == 2 and verdict("0" * 40) == 2, "a base that is not given, or not in the clone, cannot be checked")
        config = '<?xml version="1.0"?>\n<packages>\n  <package id="Foo.Bar" version="1.9.9" targetFramework="net48" />\n</packages>\n'
        reset(base)
        commit({"test/packages.config": config}, "a packages.config")
        expect(verdict(base) == 1, "packages.config's packages are read: %s" % said)
        expect(lock_kind("lockfiles/ControlPanel/packages.arm64.lock.json") == "nuget-lock",
               "the ARM64 lock files under lockfiles/ are NuGet lock files")
        # Every requirements file Renovate's pip_requirements manager updates is read.
        for path in ("build/ci/cli-packages-requirements.txt", "build/ci/requirements.txt",
                     "build/requirements-dev.txt", "tools/requirements.pip"):
            expect(lock_kind(path) == "pip", "%s is a pinned PyPI requirements file" % path)
        for path in ("build/ci/cli-packages-requirements.in", "docs/requirements-notes.md"):
            expect(lock_kind(path) is None, "%s is not a lock file" % path)
        # And every lock file of the tree is one this reads: the job runs on every change,
        # and a lock file this does not know would pass unread.
        listed = git(ROOT, "ls-files", "-z")
        if listed.returncode == 0:
            names = [p.decode("utf-8", "surrogateescape") for p in listed.stdout.split(b"\0") if p]
            pinned = [p for p in names if re.search(r"(^|/)[^/]*requirements[^/]*\.txt$", p)
                      or re.search(r"(^|/)packages[^/]*\.lock\.json$", p)]
            unread = [p for p in pinned if lock_kind(p) is None]
            expect(pinned and not unread, "every lock file of the tree is read: %s unread" % unread)
        reset(base)
        commit({"build/ci/cli-packages-requirements.txt": requirements(requests="2.0.0")},
               "a PyPI version in the command-line packages' requirements")
        expect(verdict(base) == 1 and any("GHSA-cccc-cccc-cccc" in s for s in said if s.startswith("HELD")),
               "an added version in cli-packages-requirements.txt is held: %s" % said)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--base", help="the change's base commit, or 'auto': build/ci/security-gate.py's change_base "
                                       "(CI_MERGE_REQUEST_DIFF_BASE_SHA, else where HEAD leaves master)")
    parser.add_argument("--osv-dir", help="read <ecosystem>.zip from here instead of OSV's export")
    parser.add_argument("--today", help="the date exceptions are judged by (YYYY-MM-DD; default today, UTC)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    today = None
    if args.today:
        today = gate._date(args.today)
        if not today:
            print("--today is not a date: %s" % args.today)
            return 2
    base = args.base or ""
    if base == "auto":
        base, code = gate.auto_base(ROOT)
        if code:
            return code
    own = main_check(ROOT, base, args.osv_dir, today)
    if not re.match(r"^[0-9a-f]{7,64}$", base):
        return own
    # A change to the gates' code is judged by the base's copy too (security-gate.py,
    # base_copy_verdict): one that leaves a library out of dependency-scan's list cannot
    # pass the version it moves by that.
    arguments = ["--base", base] + (["--osv-dir", os.path.abspath(args.osv_dir)] if args.osv_dir else []) + \
        (["--today", args.today] if args.today else [])
    return gate.combined(own, gate.base_copy_verdict(ROOT, base, "build/ci/mr-dependency-check.py", arguments))


if __name__ == "__main__":
    sys.exit(main())
