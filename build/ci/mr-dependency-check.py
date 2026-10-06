#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A merge request that adds a dependency version with a known High or Critical advisory is held.

The mr-dependency-check job of .gitlab/ci/mr-checks.yml runs this in a merge
request's pipeline, on GitLab's shared runners, with no secret, when the merge
request changes a dependency lock file. The dependency-scan job (Trivy and OSV,
build/ci/dependency-scan.py) reads the whole tree, but on linix, for a protected
ref only, so it first sees a dependency once it has landed: this is the check
before that, on what the change adds (Roadmap3, "Dependency check on merge
requests"). What GitHub's dependency review did until 18 September 2026.

What it reads, at the merge request's base (CI_MERGE_REQUEST_DIFF_BASE_SHA, the
target branch where the merge request left it) and at its head, from git:

  **/packages*.lock.json   NuGet's lock files - the tools', the Control Panel's, the
                           tests' - and the ARM64 ones beside them
                           (packages.arm64.lock.json), or under lockfiles/ should
                           they move there: every resolved package that is not a
                           project of the tree;
  **/packages.config       the older .NET projects' NuGet packages;
  **/*requirements*.txt    the Python tools' pinned packages (name==version), PyPI -
                           every name Renovate's pip_requirements manager updates,
                           build/ci/cli-packages-requirements.txt among them;
  **/native-dependencies*.json
                           a native component list (build/native-dependencies.ps1),
                           if one is committed: its components are C and C++
                           libraries that no package ecosystem of OSV's names, so an
                           added one is listed, and left to dependency-scan, which
                           asks OSV about each by its upstream repository's tag.

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

A Critical or High advisory against an added version holds the merge request, unless
.gitlab/security-exceptions.yml as the BASE has it accepts it: an entry of scan
dependency-scan naming the advisory (or one of its aliases) and the package, read by
build/ci/security-gate.py's own reader - never the merge request's own copy, so a
change cannot accept its own advisory. The rest is listed.

Usage, from the root of a clone of the merge request's head with its history:
  python3 build/ci/mr-dependency-check.py --base SHA [--osv-dir DIR] [--today YYYY-MM-DD]
  python3 build/ci/mr-dependency-check.py --self-test
--osv-dir reads <ecosystem>.zip from DIR instead of downloading it. Exits 0 when the
merge request may land, 1 when an added version has an advisory that holds it, 2
when the lock files or the advisories cannot be read (never a pass).
"""

import argparse
import datetime
import decimal
import http.client
import importlib.util
import io
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import xml.etree.ElementTree as ET
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXCEPTIONS = ".gitlab/security-exceptions.yml"
BLOCKING = ("Critical", "High")


def _load(relative, name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, *relative.split("/")))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The exceptions file's reader, and OSV's export as Renovate's database takes it.
gate = _load("build/ci/security-gate.py", "security_gate")
osv_offline = _load("build/ci/osv-offline-db.py", "osv_offline_db")


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


def parse_lock(kind, text):
    """[(ecosystem, name as written, version)] of one lock file. ValueError when it
    cannot be read: a lock file this cannot read is never a clean one."""
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
        for line in text.splitlines():
            line = line.split("#", 1)[0].strip()
            # Options (-r, --hash=..., --index-url) name no package of their own.
            if not line or line.startswith("-"):
                continue
            match = re.match(r"^([A-Za-z0-9][A-Za-z0-9._-]*)(\[[^\]]*\])?\s*===?\s*([^\s;\\]+)", line)
            if match:
                found.append(("PyPI", match.group(1), match.group(3)))
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


def dependencies_at(root, ref):
    """{key: [(file, name, version)]} of every lock file at ref."""
    listed = git(root, "ls-tree", "-r", "-z", "--name-only", ref)
    if listed.returncode != 0:
        raise ValueError("git ls-tree %s failed: %s" % (ref, listed.stderr.decode("utf-8", "replace").strip()[:300]))
    found = {}
    for raw in listed.stdout.split(b"\0"):
        path = raw.decode("utf-8", "surrogateescape")
        kind = lock_kind(path) if path else None
        if not kind:
            continue
        shown = git(root, "show", "%s:%s" % (ref, path))
        if shown.returncode != 0:
            raise ValueError("git show %s:%s failed" % (ref, path))
        try:
            entries = parse_lock(kind, shown.stdout.decode("utf-8"))
        except (ValueError, ET.ParseError, UnicodeDecodeError) as e:
            raise ValueError("%s at %s cannot be read: %s" % (path, ref[:12], e))
        for ecosystem, name, version in entries:
            found.setdefault(key_of(ecosystem, name, version), []).append((path, name, version))
    return found


def added_dependencies(root, base):
    """[(key, [(file, name, version)])] at HEAD and not at base."""
    before = dependencies_at(root, base)
    after = dependencies_at(root, "HEAD")
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


# ---------------------------------------------------------------- the verdict

def evaluate(added, exceptions_text, today, loader, say=print, held_versions=None):
    """The problems that hold the merge request, one per vulnerability of an added
    version; held_versions, when given, collects the versions they hold. loader(ecosystem)
    gives {id: advisory}."""
    held_versions = set() if held_versions is None else held_versions
    problems = []
    entries, wrong = gate.check_exceptions(exceptions_text)
    if wrong:
        return ["the base's %s cannot be read, so nothing is accepted: %s" % (EXCEPTIONS, w) for w in wrong]
    in_force, wrong = gate._in_force(entries, today, say)
    problems.extend(wrong)
    accepted = {}
    for e in in_force.get("dependency-scan", []):
        accepted[(e["finding"], pypi_name(e["package"]))] = e

    if not added:
        say("No dependency version is added: nothing to look up.")
        return problems
    loaded, indexes = {}, {}
    for key, where in added:
        ecosystem, name, version = key
        files = ", ".join(sorted({w[0] for w in where}))
        shown = where[0][1]
        if ecosystem == "native":
            say("NOT CHECKED HERE  %s %s (%s): a native component; OSV has no package ecosystem for it, and the "
                "dependency-scan job asks OSV about it by its upstream repository once it lands" % (shown, version, files))
            continue
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


def main_check(root, base, osv_dir=None, today=None, say=print):
    if not re.match(r"^[0-9a-f]{7,64}$", base or ""):
        say("CANNOT CHECK: no base commit (%r): give the merge request's CI_MERGE_REQUEST_DIFF_BASE_SHA" % base)
        return 2
    if git(root, "cat-file", "-e", base + "^{commit}").returncode != 0:
        say("CANNOT CHECK: the base commit %s is not in this clone: the job needs the history (GIT_DEPTH 0)" % base)
        return 2
    try:
        added = added_dependencies(root, base)
    except ValueError as e:
        say("CANNOT CHECK: %s" % e)
        return 2
    shown = git(root, "show", "%s:%s" % (base, EXCEPTIONS))
    exceptions_text = shown.stdout.decode("utf-8") if shown.returncode == 0 else "exceptions: []\n"
    today = today or datetime.datetime.now(datetime.timezone.utc).date()
    held_versions = set()
    try:
        problems = evaluate(added, exceptions_text, today, lambda e: load_ecosystem(e, osv_dir), say, held_versions)
    except (OSError, ValueError, zipfile.BadZipFile, http.client.HTTPException) as e:
        say("CANNOT CHECK: OSV's advisories could not be read (%s): no answer is not a clean answer; retry the job" % e)
        return 2
    for p in problems:
        say("HELD: " + p)
    if problems:
        advisories = sum(1 for p in problems if " [rated by " in p)
        other = len(problems) - advisories
        say("This merge request is held: %d added dependency version(s) with %d Critical or High advisory(ies) between "
            "them%s. Take a version no such advisory affects, or have the advisory accepted for the package in %s by a "
            "merge request of its own, landed first (SECURITY.md, 'Before a release')." % (
                len(held_versions), advisories,
                (", and %d problem(s) with the base's exceptions file" % other) if other else "", EXCEPTIONS))
        return 1
    say("No added dependency version has a Critical or High advisory that the base's %s does not accept." % EXCEPTIONS)
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

        empty = "exceptions: []\n"
        base = commit({"tools/A/packages.lock.json": lock(Foo_Bar="2.0.0"), "build/requirements.txt": requirements(requests="2.0.1"),
                       EXCEPTIONS: empty}, "base")

        said = []

        def verdict(base_commit, loader_dir=osv_dir):
            del said[:]
            return main_check(repo, base_commit, loader_dir, datetime.date(2026, 10, 1), say=said.append)

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
        commit({"tools/A/packages.lock.json": lock(Foo_Bar="1.5.0"),
                "tools/A/native-dependencies-arm64.json": json.dumps([{"name": "OpenSSL", "version": "4.0.2"}])},
               "a native list and a downgrade")
        expect(verdict(base) == 1 and any(s.startswith("NOT CHECKED HERE  OpenSSL 4.0.2") for s in said),
               "a native component is named as not checked here: %s" % said)
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
        # And every lock file of the tree is one this reads, so the job's rules:changes
        # (.gitlab/ci/mr-checks.yml) and this agree on what a lock file is.
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
    parser.add_argument("--base", help="the merge request's base commit (CI_MERGE_REQUEST_DIFF_BASE_SHA)")
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
    return main_check(ROOT, args.base or "", args.osv_dir, today)


if __name__ == "__main__":
    sys.exit(main())
