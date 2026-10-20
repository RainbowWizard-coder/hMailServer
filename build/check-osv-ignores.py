#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Each advisory an osv-scanner.toml ignores has its not_affected statement in the VEX
document, is still pinned beside it, and is ignored until a day still to come.

OpenSSF Scorecard's Vulnerabilities check runs OSV-Scanner over a checkout of the tree -
scorecard.dev's runs and .github/workflows/scorecard.yml's over the copy on GitHub, and a
run with --local; not GitLab's scorecard job, whose Scorecard GitLab client gives the check
no local path, so that OSV-Scanner is asked about the commit alone there - and OSV-Scanner
reads the osv-scanner.toml in the directory of each manifest it scans. Its [[IgnoredVulns]] entries hide an advisory from
that check, and from nothing else: the project's record of the decision is the advisory's
statement in .gitlab/hmailserver.openvex.json, which the dependency-scan job and the merge
requests' dependency check read, and which OSV-Scanner, as Scorecard runs it, does not.
The two are written by hand, in two places, and nothing else holds them together: an
entry with no statement hides an advisory no one has declared, and an entry left behind
when the package moved hides the next advisory with that id from a version no one read.
On 20 October 2026 build/ci/osv-scanner.toml took its first entry, mcp 1.29.0's
GHSA-84m7-p3x7-pcfv, which semgrep 1.181.0 pins and the VEX document had declared
not_affected the day before.

For every osv-scanner.toml git tracks (every one under the tree, when it is not a git work
tree), this fails when the file does not parse as TOML, holds another table than
[[IgnoredVulns]] - PackageOverrides, say, which can hide a package's advisories with no
id at all - or an entry
  - with no id, no reason, no ignoreUntil, or an id named twice;
  - whose ignoreUntil, a TOML date or date-time, is today or before (UTC): OSV-Scanner
    reports the advisory again from that day, and Scorecard's score falls with no change
    to the tree, so the date is moved here, by a change that reads the statement again,
    before it comes;
  - whose id is neither the name nor an alias of a not_affected statement of the VEX
    document (a fixed or affected statement does not count: the advisory is ignored only
    where it is declared not to apply);
  - for which no manifest in the toml's directory - a requirements file OSV-Scanner
    reads there, *requirements*.txt - pins, name==version, a PyPI package version one of
    those statements' products names (pkg:pypi/<name>@<version>): the package has moved
    on, and the entry, and the statement once nothing pins that version, go.

  python build/check-osv-ignores.py              the tree
  python build/check-osv-ignores.py --self-test  against fixtures

Exit codes: 0 every entry holds; 1 one does not, or a file cannot be read; 2 the VEX
document cannot be read.
"""

import os
import sys

# Nothing is imported by name from this script's own directory, which Python puts first
# on sys.path (build/check-release-chain.py, PATH_GUARD).
sys.path[:] = [p for p in sys.path if os.path.normcase(os.path.realpath(p or os.curdir)) !=
               os.path.normcase(os.path.dirname(os.path.realpath(__file__)))]

import argparse
import datetime
import fnmatch
import json
import re
import shutil
import subprocess
import tempfile
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VEX = ".gitlab/hmailserver.openvex.json"
CONFIG = "osv-scanner.toml"
# The manifests OSV-Scanner's requirements extractor reads in a directory, the one
# ecosystem whose manifests sit beside an osv-scanner.toml in this tree.
MANIFEST = "*requirements*.txt"
KEYS = {"id", "ignoreUntil", "reason"}
# name==version at the start of a requirements line, extras allowed (PEP 508).
PIN = re.compile(r"^([A-Za-z0-9][A-Za-z0-9._-]*)(?:\[[^\]]*\])?\s*==\s*([^\s;#\\]+)")
PURL = re.compile(r"^pkg:pypi/([^@?#/]+)@([^?#]+)")


def pypi_name(name):
    """A PyPI project name as PEP 503 compares it."""
    return re.sub(r"[-_.]+", "-", name).lower()


def configs(root):
    """The tree's osv-scanner.toml files, as paths relative to root with '/'."""
    top = subprocess.run(["git", "-C", root, "rev-parse", "--show-toplevel"], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    same = lambda a, b: os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))
    if top.returncode == 0 and same(top.stdout.decode("utf-8", "replace").strip(), root):
        listed = subprocess.run(["git", "-C", root, "-c", "core.quotePath=false", "ls-files", "-z", "--",
                                 ":(glob)**/" + CONFIG, CONFIG], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if listed.returncode == 0:
            return sorted(set(p for p in listed.stdout.decode("utf-8", "replace").split("\0") if p))
    found = []
    for base, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if d != ".git"]
        if CONFIG in names:
            found.append(os.path.relpath(os.path.join(base, CONFIG), root).replace(os.sep, "/"))
    return sorted(found)


def not_affected(text):
    """{advisory name or alias: [(pypi name, version)] of its not_affected statements'
    products}. ValueError when the text is not an OpenVEX document."""
    document = json.loads(text.lstrip("\ufeff"))
    if not isinstance(document, dict) or not isinstance(document.get("statements"), list):
        raise ValueError("not an OpenVEX document: no list of statements")
    found = {}
    for statement in document["statements"]:
        if not isinstance(statement, dict) or statement.get("status") != "not_affected":
            continue
        vulnerability = statement.get("vulnerability") or {}
        if not isinstance(vulnerability, dict):
            continue
        names = [str(n) for n in [vulnerability.get("name")] + list(vulnerability.get("aliases") or []) if n]
        products = []
        for product in statement.get("products") or []:
            entries = [product] + (list(product.get("subcomponents") or []) if isinstance(product, dict) else [])
            for entry in entries:
                ids = [entry] if isinstance(entry, str) else (
                    [entry.get("@id"), (entry.get("identifiers") or {}).get("purl")] if isinstance(entry, dict) else [])
                for value in ids:
                    match = PURL.match(value) if isinstance(value, str) else None
                    if match and (pypi_name(match.group(1)), match.group(2)) not in products:
                        products.append((pypi_name(match.group(1)), match.group(2)))
        for name in names:
            found.setdefault(name, [])
            found[name] += [p for p in products if p not in found[name]]
    return found


def pins(directory):
    """{(pypi name, version): manifest file name} of the requirements files in directory."""
    found = {}
    try:
        names = sorted(os.listdir(directory))
    except OSError:
        return found
    for name in names:
        if not fnmatch.fnmatch(name.lower(), MANIFEST) or not os.path.isfile(os.path.join(directory, name)):
            continue
        with open(os.path.join(directory, name), "rb") as f:
            text = f.read().decode("utf-8", "replace")
        for line in text.splitlines():
            match = PIN.match(line.strip())
            if match:
                found.setdefault((pypi_name(match.group(1)), match.group(2)), name)
    return found


def until_day(value):
    """The UTC day an ignoreUntil value ends on, or None when it is no date."""
    if isinstance(value, datetime.datetime):
        if value.tzinfo is not None:
            value = value.astimezone(datetime.timezone.utc)
        return value.date()
    if isinstance(value, datetime.date):
        return value
    return None


def problems_of(root, path, statements, today):
    """The problems of one osv-scanner.toml, and its number of entries."""
    full = os.path.join(root, *path.split("/"))
    try:
        with open(full, "rb") as f:
            config = tomllib.loads(f.read().decode("utf-8").lstrip("\ufeff"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as e:
        return ["%s cannot be read as TOML: %s" % (path, e)], 0
    problems = []
    for key in sorted(set(config) - {"IgnoredVulns"}):
        problems.append("%s holds %r, which this check does not read: an advisory is ignored only by an "
                        "[[IgnoredVulns]] entry that names it and its not_affected statement in %s" % (path, key, VEX))
    entries = config.get("IgnoredVulns", [])
    if not isinstance(entries, list):
        return problems + ["%s: IgnoredVulns is not an array of tables ([[IgnoredVulns]])" % path], 0
    beside = pins(os.path.dirname(full))
    seen = set()
    for number, entry in enumerate(entries, 1):
        if not isinstance(entry, dict):
            problems.append("%s: IgnoredVulns entry %d is not a table" % (path, number))
            continue
        vid = entry.get("id")
        where = "%s: the entry %s" % (path, ("%r" % vid) if isinstance(vid, str) and vid else "number %d" % number)
        for key in sorted(set(entry) - KEYS):
            problems.append("%s holds %r, which this check does not read (only %s)" % (where, key, ", ".join(sorted(KEYS))))
        if not isinstance(vid, str) or not vid.strip():
            problems.append("%s has no id" % where)
            continue
        if vid in seen:
            problems.append("%s is named twice" % where)
        seen.add(vid)
        reason = entry.get("reason")
        if not isinstance(reason, str) or not reason.strip():
            problems.append("%s has no reason: say in one sentence why it does not apply, as its statement in %s "
                            "does, and when the entry goes" % (where, VEX))
        if "ignoreUntil" not in entry:
            problems.append("%s has no ignoreUntil: an entry ends on a day, by which it is read again" % where)
        else:
            day = until_day(entry["ignoreUntil"])
            if day is None:
                problems.append("%s has an ignoreUntil that is no TOML date or date-time: %r" % (where, entry["ignoreUntil"]))
            elif day <= today:
                problems.append("%s was ignored until %s, and today is %s (UTC): OSV-Scanner reports it again, so read "
                                "its statement in %s again, and move the day on or take the entry out"
                                % (where, day.isoformat(), today.isoformat(), VEX))
        products = statements.get(vid)
        if products is None:
            problems.append("%s names an advisory that no not_affected statement of %s names, by its name or an "
                            "alias: write the statement first, or take the entry out" % (where, VEX))
            continue
        pinned = sorted("%s==%s (%s)" % (name, version, beside[(name, version)])
                        for name, version in products if (name, version) in beside)
        if not pinned:
            covered = ", ".join("%s %s" % p for p in products) or "no PyPI package"
            problems.append("%s: no manifest in %s (%s) pins a version its not_affected statement covers (%s): the "
                            "package has moved on, so take the entry out, and the statement once nothing pins that "
                            "version" % (where, os.path.dirname(path) or ".", MANIFEST, covered))
    return problems, len(entries)


def check(root=ROOT, say=print, today=None):
    today = today or datetime.datetime.now(datetime.timezone.utc).date()
    try:
        with open(os.path.join(root, *VEX.split("/")), "rb") as f:
            statements = not_affected(f.read().decode("utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as e:
        say("check-osv-ignores: cannot read %s: %s" % (VEX, e))
        return 2
    files = configs(root)
    problems, count = [], 0
    for path in files:
        found, entries = problems_of(root, path, statements, today)
        problems += found
        count += entries
    for problem in problems:
        say("FAIL: " + problem)
    if problems:
        say("check-osv-ignores: %d problem(s) in the tree's %s files" % (len(problems), CONFIG))
        return 1
    say("check-osv-ignores: %d ignored advisor%s in %d %s file(s) (%s), each with its not_affected statement in %s, "
        "a version it covers pinned beside it, and a day still to come"
        % (count, "y" if count == 1 else "ies", len(files), CONFIG, ", ".join(files) or "none", VEX))
    return 0


def self_test():
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    today = datetime.date(2026, 10, 20)
    work = tempfile.mkdtemp(prefix="check-osv-ignores-")
    try:
        def statement(name, aliases, purls, status="not_affected"):
            return {"vulnerability": {"name": name, "aliases": aliases}, "status": status,
                    "products": [{"@id": p, "identifiers": {"purl": p}} for p in purls]}

        vex = {"@context": "https://openvex.dev/ns/v0.2.0", "statements": [
            statement("CVE-2026-59951", ["GHSA-84m7-p3x7-pcfv"], ["pkg:pypi/mcp@1.29.0"]),
            statement("GHSA-aaaa-bbbb-cccc", [], ["pkg:pypi/Some_Package@2.0.0"]),
            statement("GHSA-fixd-fixd-fixd", [], ["pkg:pypi/mcp@1.29.0"], status="fixed"),
            statement("GHSA-gnrc-gnrc-gnrc", [], ["pkg:generic/zlib@1.3.1"]),
        ]}
        requirements = ("# a header\nanyio==4.15.1 \\\n    --hash=sha256:%s\nmcp==1.29.0 \\\n    --hash=sha256:%s\n"
                        "    # via semgrep\nsome-package[extra]==2.0.0\n" % ("1" * 64, "2" * 64))
        good = ('# a comment\n[[IgnoredVulns]]\nid = "GHSA-84m7-p3x7-pcfv"\nignoreUntil = 2027-01-20\n'
                'reason = "not reached."\n')

        def tree(config, manifest=requirements, document=None, where="build/ci", name="requirements.txt", when=today):
            shutil.rmtree(work, ignore_errors=True)
            files = {VEX: json.dumps(vex if document is None else document)}
            if config is not None:
                files[where + "/" + CONFIG] = config
            if manifest is not None:
                files[where + "/" + name] = manifest
            for path, text in files.items():
                full = os.path.join(work, *path.split("/"))
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "w", encoding="utf-8", newline="") as f:
                    f.write(text)
            said = []
            return check(work, said.append, when), "\n".join(said)

        code, text = tree(good)
        expect(code == 0 and "1 ignored advisory in 1 osv-scanner.toml file(s) (build/ci/osv-scanner.toml)" in text,
               "an entry with its statement, its pin and a day to come passes: %s %s" % (code, text))
        code, text = tree(good.replace("\n", "\r\n"), requirements.replace("\n", "\r\n"))
        expect(code == 0, "files checked out CRLF are read alike: %s %s" % (code, text))
        code, text = tree(good.replace('"GHSA-84m7-p3x7-pcfv"', '"CVE-2026-59951"'))
        expect(code == 0, "an entry that names the statement by its name, not its alias, passes: %s" % text)
        code, text = tree(good.replace("2027-01-20", "2027-01-20T00:00:00Z"))
        expect(code == 0, "a date-time passes: %s" % text)
        code, text = tree(good + '\n[[IgnoredVulns]]\nid = "GHSA-aaaa-bbbb-cccc"\nignoreUntil = 2027-01-01\nreason = "r."\n')
        expect(code == 0 and "2 ignored advisories" in text,
               "a PyPI name is compared as PEP 503 has it, extras and all: %s" % text)
        code, text = tree(None)
        expect(code == 0 and "0 ignored advisories in 0 osv-scanner.toml file(s) (none)" in text,
               "a tree with no osv-scanner.toml passes, saying so: %s" % text)
        # The day.
        code, text = tree(good, when=datetime.date(2027, 1, 20))
        expect(code == 1 and "was ignored until 2027-01-20, and today is 2027-01-20" in text,
               "an entry on its last day fails: OSV-Scanner reports it from that day: %s" % text)
        code, text = tree(good, when=datetime.date(2027, 3, 1))
        expect(code == 1 and "was ignored until 2027-01-20" in text, "an entry past its day fails: %s" % text)
        code, text = tree(good, when=datetime.date(2027, 1, 19))
        expect(code == 0, "an entry the day before its day passes: %s" % text)
        code, text = tree(good.replace("ignoreUntil = 2027-01-20\n", ""))
        expect(code == 1 and "has no ignoreUntil" in text, "an entry with no day fails: %s" % text)
        code, text = tree(good.replace("2027-01-20", '"2027-01-20"'))
        expect(code == 1 and "no TOML date or date-time" in text, "a day written as a string fails: %s" % text)
        # The statement.
        code, text = tree(good.replace("GHSA-84m7-p3x7-pcfv", "GHSA-none-none-none"))
        expect(code == 1 and "no not_affected statement of %s names" % VEX in text,
               "an entry with no statement fails: %s" % text)
        code, text = tree(good.replace("GHSA-84m7-p3x7-pcfv", "GHSA-fixd-fixd-fixd"))
        expect(code == 1 and "no not_affected statement" in text, "a fixed statement is no not_affected one: %s" % text)
        affected = json.loads(json.dumps(vex))
        affected["statements"][0]["status"] = "affected"
        code, text = tree(good, document=affected)
        expect(code == 1 and "no not_affected statement" in text, "an affected statement does not count: %s" % text)
        # The pin.
        code, text = tree(good, requirements.replace("mcp==1.29.0", "mcp==1.30.0"))
        expect(code == 1 and "no manifest in build/ci" in text and "pins a version its not_affected statement covers "
               "(mcp 1.29.0)" in text, "an entry whose package has moved on fails: %s" % text)
        code, text = tree(good, None)
        expect(code == 1 and "no manifest in build/ci" in text, "an entry with no manifest beside it fails: %s" % text)
        code, text = tree(good, requirements, name="cli-packages-requirements.txt")
        expect(code == 0, "any requirements file beside the toml is a manifest: %s" % text)
        code, text = tree(good, requirements, name="constraints.txt")
        expect(code == 1, "a file OSV-Scanner does not read as a manifest pins nothing: %s" % text)
        code, text = tree(good.replace("GHSA-84m7-p3x7-pcfv", "GHSA-gnrc-gnrc-gnrc"))
        expect(code == 1 and "(no PyPI package)" in text, "a statement of no PyPI package pins nothing here: %s" % text)
        code, text = tree(good, where="elsewhere")
        expect(code == 0 and "elsewhere/osv-scanner.toml" in text, "a toml in another directory is read too: %s" % text)
        # The file's shape.
        code, text = tree(good.replace('reason = "not reached."\n', ""))
        expect(code == 1 and "has no reason" in text, "an entry with no reason fails: %s" % text)
        code, text = tree(good.replace('reason = "not reached."', 'reason = "  "'))
        expect(code == 1 and "has no reason" in text, "an empty reason fails: %s" % text)
        code, text = tree(good + good)
        expect(code == 1 and "is named twice" in text, "an id named twice fails: %s" % text)
        code, text = tree(good.replace('id = "GHSA-84m7-p3x7-pcfv"\n', ""))
        expect(code == 1 and "has no id" in text, "an entry with no id fails: %s" % text)
        code, text = tree(good + '\n[[PackageOverrides]]\nname = "mcp"\necosystem = "PyPI"\nignore = true\n')
        expect(code == 1 and "holds 'PackageOverrides', which this check does not read" in text,
               "a package override, which hides advisories with no id, fails: %s" % text)
        code, text = tree(good.replace("reason =", 'effectiveUntil = 2027-01-01\nreason ='))
        expect(code == 1 and "holds 'effectiveUntil'" in text, "a key of an entry that is not read fails: %s" % text)
        code, text = tree("[[IgnoredVulns]\nid = 1\n")
        expect(code == 1 and "cannot be read as TOML" in text, "a file that is no TOML fails: %s" % text)
        code, text = tree(good, document={"statements": "none"})
        expect(code == 2 and "cannot read %s" % VEX in text, "a VEX document that cannot be read is exit 2: %s" % text)
        expect(PIN.match("mcp==1.29.0 \\") and PIN.match("mcp==1.29.0 \\").group(2) == "1.29.0",
               "a pin continued onto its hashes reads its version alone")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    # In a git work tree only the files git tracks are read.
    repo = tempfile.mkdtemp(prefix="check-osv-ignores-git-")
    try:
        subprocess.run(["git", "init", "-q", repo], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for name in ("a/" + CONFIG, "b/" + CONFIG):
            os.makedirs(os.path.join(repo, os.path.dirname(name)), exist_ok=True)
            with open(os.path.join(repo, *name.split("/")), "w", encoding="utf-8") as f:
                f.write("")
        subprocess.run(["git", "-C", repo, "add", "a/" + CONFIG], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        found = configs(repo)
        expect(found == ["a/" + CONFIG], "in a git work tree the tracked toml alone is read: %s" % found)
    finally:
        for base, _, names in os.walk(repo):
            for name in names:
                try:
                    os.chmod(os.path.join(base, name), 0o600)
                except OSError:
                    pass
        shutil.rmtree(repo, ignore_errors=True)

    # The tree's own files pass, today.
    said = []
    code = check(ROOT, said.append)
    expect(code == 0 and "ignored advisor" in "\n".join(said), "the tree passes: %s" % said)

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--root", default=ROOT, help="the tree (default: this script's)")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    return check(os.path.abspath(args.root))


if __name__ == "__main__":
    sys.exit(main())
