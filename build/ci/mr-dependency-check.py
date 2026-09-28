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
in database_specific.severity (CRITICAL, HIGH, MODERATE, LOW); the CVSS v3 base
score of its vector, as FIRST's specification computes it; the highest of those of
its aliases in the same export. One with none of those is listed as unrated and holds
nothing, as the release's gate does not hold one (SECURITY.md's threshold is High).

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
    """Whether one affected[] entry of an advisory holds version."""
    mine = version_key(ecosystem, version)
    for listed in affected.get("versions") or []:
        if listed == version or (mine is not None and version_key(ecosystem, listed) == mine):
            return True
    if mine is None:
        return False
    for span in affected.get("ranges") or []:
        if span.get("type") not in ("ECOSYSTEM", "SEMVER"):
            continue
        # OSV's evaluation: the events in version order, "introduced" opening the
        # range and "fixed" (at or below the version) or "last_affected" (below it)
        # closing it again; "introduced": "0" is before every version. A range with
        # a version this cannot order is left to the "versions" list above.
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
    return False


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
    for entry in advisory.get("severity") or []:
        if entry.get("type") in ("CVSS_V3",):
            score = cvss3_score(str(entry.get("score") or ""))
            if score is not None and rating_of_score(score):
                return rating_of_score(score), "CVSS %.1f" % score
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


def advisories_for(ecosystem, name, version, index):
    """The advisories that affect one version of one package, not withdrawn."""
    wanted = key_of(ecosystem, name, version)[1]
    hits = []
    for advisory in index.values():
        if advisory.get("withdrawn"):
            continue
        for affected in advisory.get("affected") or []:
            package = affected.get("package") or {}
            if package.get("ecosystem") != ecosystem or key_of(ecosystem, package.get("name") or "", "")[1] != wanted:
                continue
            if affects(ecosystem, affected, version):
                hits.append(advisory)
                break
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
        hits = advisories_for(ecosystem, shown, version, candidates)
        if not hits:
            say("ok      %s %s %s (%s): no advisory" % (ecosystem, shown, version, files))
            continue
        for group in alias_groups(hits):
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
            line = "%s %s %s (%s): %s %s (%s)%s - %s" % (
                ecosystem, shown, version, files, level or "unrated", advisory["id"], ", ".join(others) or "no alias",
                (" - fixed in %s" % ", ".join(fixed[:4])) if fixed else " - no fixed version",
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

    def advisory(id, ecosystem, name, versions=None, ranges=None, severity=None, cvss=None, aliases=(), withdrawn=None):
        record = {"id": id, "summary": "%s in %s" % (id, name), "aliases": list(aliases),
                  "affected": [{"package": {"ecosystem": ecosystem, "name": name}, "versions": versions or [],
                                "ranges": ranges or []}]}
        if severity:
            record["database_specific"] = {"severity": severity}
        if cvss:
            record["severity"] = [{"type": "CVSS_V3", "score": cvss}]
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
                      cvss="CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H")]
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
