#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Known vulnerabilities in what the server is built from: the dependency-scan job.

What replaced GitHub's dependency review (the GitHub workflow dependency-review.yml)
and Dependabot's alerts. Three scans, one report:

  1. The lock files, with Trivy (trivy fs): the eleven NuGet packages.lock.json,
     the three packages.config, and build/ci/security-requirements.txt, the
     scanners' own Python packages. Nothing else in the tree is a manifest: the
     Python tools use the standard library (the three that do not are named by
     repo-hygiene.py), and the Node harnesses install no package.
  2. The CI image, with Trivy (trivy rootfs), from the package database the
     ci-image-packages job copies out of it (Ubuntu's dpkg status and the .NET
     SDK's deps.json files). The Linux server is built in that image and links
     its OpenSSL, libpq, libldap and, statically, its Boost, so this is also the
     Linux packages' native dependencies.
  3. The libraries the Windows server is built with, which no manifest names:
     OpenSSL, Boost and libpq at the versions hMailServer.vcxproj's include paths
     carry, zlib and QuickJS from their vendored sources, and MariaDB
     Connector/C, 7-Zip and SQL Server Compact from third-party-binaries.json -
     the same places build/native-dependencies.ps1 reads for the SBOM, so the
     version asked about is the version built. Each is asked of OSV
     (api.osv.dev) by its upstream repository's tag. OSV's C and C++ data is
     partial, so each library is also asked about a CONTROL version that a
     recent advisory is known to name: when OSV says nothing about the control
     either, the answer for the real version is UNKNOWN, never clean.

Where it comes from - nothing from GitHub. Trivy is Docker Hub's aquasec/trivy,
pinned by digest in .gitlab-ci.yml. Its vulnerability database is an OCI artifact
that Trivy pulls from ghcr.io unless told otherwise; this downloads it from
Google's mirror.gcr.io, or from AWS's public.ecr.aws when that fails, one at a
time and by name (Trivy's own list moves on only after errors it takes for
temporary, and not when a host cannot be reached: measured), and scans with the
download it has. A run that can reach neither fails; it never tries GitHub. OSV is Google's
api.osv.dev. (Both databases carry, among their sources, GitHub's advisory
database; what is avoided is GitHub as a place to download from, not its data.)

A finding that does not affect the project is dismissed in the VEX document,
.gitlab/hmailserver.openvex.json, as SECURITY.md says: Trivy reads it with --vex
and the OSV step drops every advisory the document says is not_affected.

Writes to --out (default security/dependency-scan): the tools' raw output, a
GitLab dependency-scanning report per scanner (gl-dependency-scanning-trivy.json,
gl-dependency-scanning-osv.json) and one with both (gl-dependency-scanning.json,
the job's report: GitLab takes one file per report type, and refused the upload
of two on the first pipeline, "only one file can be sent as raw"), junit.xml (a
test case per scanned target - the part GitLab's free tier shows) and
findings.txt. Exits 1 when a high or
critical vulnerability with a fixed version is found (SECURITY.md's threshold:
fixed before the next release), 2 when a scan could not run, 0 otherwise.

Usage, from the root of a checkout, in the aquasec/trivy image with python3:

  python3 build/ci/dependency-scan.py [--image-root DIR] [--out DIR] [--skip-trivy] [--skip-osv]

--image-root is the directory the ci-image-packages job left (ci-image-packages/
by default; that scan is skipped, and says so, when it is not there).
"""

import argparse
import datetime
import json
import math
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
import uuid
from xml.sax.saxutils import escape, quoteattr

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SCHEMA_VERSION = "15.2.5"
ID_NAMESPACE = uuid.UUID("0f6a8d3e-4c1b-5e2a-8d7f-6b9c2e1a4f03")
VEX = os.path.join(".gitlab", "hmailserver.openvex.json")
OSV_API = "https://api.osv.dev/v1"

# The vulnerability database's mirrors, tried in order, one at a time. Trivy's
# default list ends with ghcr.io; naming a repository replaces the default, so
# ghcr.io is never tried.
TRIVY_DB = ["mirror.gcr.io/aquasec/trivy-db:2", "public.ecr.aws/aquasecurity/trivy-db:2"]
# Read only for a .jar, and the tree has none; named so that one would not reach ghcr.io.
TRIVY_JAVA_DB = "mirror.gcr.io/aquasec/trivy-java-db:1,public.ecr.aws/aquasecurity/trivy-java-db:1"

VCXPROJ = "hmailserver/source/Server/hMailServer/hMailServer.vcxproj"
BINARIES = "hmailserver/docs/third-party-binaries.json"

# The libraries no manifest names. For each: where its version is read, how OSV
# names it (the upstream repository and the tag of a version), and a CONTROL
# version that a recent advisory in OSV is known to name, so that "no advisory"
# can be told apart from "no data". oldest_tag is the first version the upstream
# repository has a tag for, where that is later than versions still in use: OSV
# matches a version by its tag, and cannot match one the repository never had.
VENDORED = [
    {"name": "OpenSSL", "file": VCXPROJ, "pattern": r"openssl-(\d+\.\d+\.\d+[a-z]?)",
     "repo": "https://github.com/openssl/openssl", "tag": "openssl-{v}", "control": "4.0.0",
     "used": "TLS for every listener and the REST API, DKIM, hashing (Windows build)"},
    {"name": "Boost", "file": VCXPROJ, "pattern": r"boost_(\d+)_(\d+)_(\d+)",
     "repo": "https://github.com/boostorg/boost", "tag": "boost-{v}", "control": "1.90.0",
     "used": "Asio under all socket I/O, threads, filesystem, regex (Windows build)",
     "coverage": "OSV's Boost records are OSS-Fuzz crash reports only; Boost's CVEs are not among them"},
    {"name": "libpq (PostgreSQL client)", "file": VCXPROJ, "pattern": r"postgresql-(\d+)\.(\d+)",
     "repo": "https://github.com/postgres/postgres", "tag": "REL_{v_}", "control": "17.2",
     "used": "the PostgreSQL backend's client library (Windows build)"},
    {"name": "zlib", "file": "hmailserver/source/Server/zlib/zlib.h", "pattern": r'#define ZLIB_VERSION "([0-9.]+)"',
     "repo": "https://github.com/madler/zlib", "tag": "v{v}", "control": "1.3",
     "used": "vendored source: IMAP COMPRESS=DEFLATE and the message store's compression (Windows build)"},
    {"name": "QuickJS", "file": "hmailserver/source/Server/quickjs/VERSION", "pattern": r"^\s*(\d{4}-\d{2}-\d{2})\s*$",
     "repo": "https://github.com/bellard/quickjs", "tag": "{v}", "control": "2021-03-27",
     "used": "vendored source: the event scripts' engine on the Linux builds"},
    {"name": "MariaDB Connector/C", "file": BINARIES, "component": "MariaDB Connector/C",
     "repo": "https://github.com/mariadb-corporation/mariadb-connector-c", "tag": "v{v}", "control": "3.1.7",
     "used": "libmysql.dll, the MySQL and MariaDB backends' client, shipped by the installer"},
    {"name": "7-Zip (7za.exe)", "file": BINARIES, "component": "7-Zip standalone console (7za.exe)",
     "repo": "https://github.com/ip7z/7zip", "tag": "{v}", "control": "24.08", "oldest_tag": "22.01",
     "used": "creates and restores backups (Compression.cpp), shipped by the installer",
     "coverage": "OSV's 7-Zip data is partial: CVE-2023-31102 (a crafted 7z archive and the PPMd decoder) "
                 "is in OSV only with a range it could not resolve to the repository, so no query by version finds it"},
    {"name": "SQL Server Compact 4.0", "file": BINARIES, "component": "Microsoft SQL Server Compact 4.0 SP1 x64 ENU",
     "repo": None, "used": "the built-in database, shipped by the installer",
     "coverage": "no open vulnerability database tracks it; Microsoft ended its support in July 2021"},
]


def now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as f:
        return f.read()


def version_key(v):
    return [int(x) if x.isdigit() else x for x in re.split(r"[.\-]", v)]


# ---------------------------------------------------------------- the VEX document

def vex_not_affected():
    """{vulnerability id or alias: (product, justification)} for every not_affected statement."""
    dismissed = {}
    try:
        doc = json.loads(read(VEX))
    except (OSError, ValueError):
        return dismissed
    for s in doc.get("statements", []):
        if s.get("status") not in ("not_affected", "fixed"):
            continue
        v = s.get("vulnerability", {})
        names = [v.get("name", "")] + list(v.get("aliases", []))
        products = [p.get("@id", "") if isinstance(p, dict) else str(p) for p in s.get("products", [])]
        for n in names:
            if n:
                dismissed[n] = (", ".join(products), s.get("justification") or s.get("impact_statement", ""))
    return dismissed


# ---------------------------------------------------------------- Trivy

def lock_file_packages(path):
    """How many packages a lock file or manifest names, read without Trivy."""
    text = read(path)
    if path.endswith("packages.lock.json"):
        try:
            data = json.loads(text)
        except ValueError:
            return 0
        # A "Project" entry is a reference to another project of this repository.
        return sum(1 for v in data.get("dependencies", {}).values() if isinstance(v, dict)
                   for entry in v.values() if isinstance(entry, dict) and entry.get("type") != "Project")
    if path.endswith("packages.config"):
        return len(re.findall(r"<package\s", text))
    return len(re.findall(r"^[A-Za-z0-9_.\-]+==", text, re.M))


def download_database(log):
    """The repository the vulnerability database came from, or None when none answered."""
    for repository in TRIVY_DB:
        command = ["trivy", "image", "--download-db-only", "--db-repository", repository,
                   "--skip-version-check", "--disable-telemetry", "--no-progress"]
        print("$ " + " ".join(command))
        with open(log, "a", encoding="utf-8") as out:
            if subprocess.run(command, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT).returncode == 0:
                return repository
        print("  %s did not answer; see trivy.log" % repository)
    return None


def trivy(args, output, log):
    command = ["trivy"] + args + [
        "--scanners", "vuln", "--format", "json", "--output", output,
        "--skip-db-update", "--java-db-repository", TRIVY_JAVA_DB,
        "--skip-version-check", "--disable-telemetry", "--no-progress", "--timeout", "20m",
    ]
    if os.path.exists(os.path.join(ROOT, VEX)):
        command += ["--vex", VEX, "--show-suppressed"]
    print("$ " + " ".join(command))
    with open(log, "a", encoding="utf-8") as out:
        code = subprocess.run(command, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT).returncode
    return code


def trivy_findings(path, label, file_prefix=""):
    """(findings, packages) from a Trivy JSON report."""
    with open(path, encoding="utf-8") as f:
        report = json.load(f)
    findings, packages = [], []
    for result in report.get("Results", []) or []:
        target = file_prefix + result.get("Target", "")
        for p in result.get("Packages", []) or []:
            packages.append((target, result.get("Type", ""), p.get("Name"), p.get("Version")))
        for v in result.get("Vulnerabilities", []) or []:
            findings.append({
                "scanner": "trivy", "scan": label, "file": target, "type": result.get("Type", ""),
                "package": v.get("PkgName", ""), "version": v.get("InstalledVersion", ""),
                "id": v.get("VulnerabilityID", ""), "severity": (v.get("Severity") or "UNKNOWN").title(),
                "fixed": v.get("FixedVersion", "") or "", "title": v.get("Title", "") or "",
                "description": (v.get("Description") or "")[:2000],
                "url": v.get("PrimaryURL", "") or "", "status": v.get("Status", ""),
            })
        for v in result.get("ExperimentalModifiedFindings", []) or []:
            # --show-suppressed: what the VEX document dismissed, kept for the record.
            finding = v.get("Finding", {})
            findings.append({
                "scanner": "trivy", "scan": label, "file": target, "type": result.get("Type", ""),
                "package": finding.get("PkgName", ""), "version": finding.get("InstalledVersion", ""),
                "id": finding.get("VulnerabilityID", ""), "severity": (finding.get("Severity") or "UNKNOWN").title(),
                "fixed": finding.get("FixedVersion", "") or "", "title": finding.get("Title", "") or "",
                "description": "", "url": finding.get("PrimaryURL", "") or "",
                "status": "dismissed in the VEX document: " + str(v.get("Statement", "")),
                "dismissed": True,
            })
    return findings, packages


# ---------------------------------------------------------------- OSV, for the vendored libraries

def osv_query(repo, tag):
    body = json.dumps({"package": {"name": repo, "ecosystem": "GIT"}, "version": tag}).encode("utf-8")
    vulns, token = [], None
    while True:
        payload = json.loads(body)
        if token:
            payload["page_token"] = token
        request = urllib.request.Request(OSV_API + "/query", data=json.dumps(payload).encode("utf-8"),
                                         headers={"Content-Type": "application/json",
                                                  "User-Agent": "hmailserver-dependency-scan"})
        with urllib.request.urlopen(request, timeout=60) as response:
            answer = json.load(response)
        vulns += answer.get("vulns", []) or []
        token = answer.get("next_page_token")
        if not token:
            return vulns


def vendored_version(lib):
    if "component" in lib:
        data = json.loads(read(lib["file"]))
        for a in data.get("artifacts", []):
            if a.get("component") == lib["component"]:
                return str(a.get("version", "")).split()[0], lib["file"]
        return None, lib["file"]
    m = re.search(lib["pattern"], read(lib["file"]), re.M)
    if not m:
        return None, lib["file"]
    return ".".join(m.groups()), lib["file"]


def osv_severity(v):
    for s in v.get("severity", []) or []:
        score = str(s.get("score", ""))
        if score.startswith("CVSS:3."):
            return cvss3_rating(score)
    rating = (v.get("database_specific") or {}).get("severity")
    if isinstance(rating, str) and rating.title() in ("Critical", "High", "Medium", "Low"):
        return rating.title()
    if v.get("id", "").startswith("OSV-"):
        return "Medium"  # an OSS-Fuzz crash: memory safety, severity not rated
    return "Unknown"


def cvss3_rating(vector):
    """The CVSS 3.x base score's rating, computed from the vector (no library)."""
    m = dict(part.split(":", 1) for part in vector.split("/")[1:] if ":" in part)
    try:
        av = {"N": 0.85, "A": 0.62, "L": 0.55, "P": 0.2}[m["AV"]]
        ac = {"L": 0.77, "H": 0.44}[m["AC"]]
        ui = {"N": 0.85, "R": 0.62}[m["UI"]]
        changed = m["S"] == "C"
        pr = {"N": 0.85, "L": 0.68 if changed else 0.62, "H": 0.5 if changed else 0.27}[m["PR"]]
        cia = [{"H": 0.56, "L": 0.22, "N": 0.0}[m[k]] for k in ("C", "I", "A")]
    except KeyError:
        return "Unknown"
    iss = 1 - (1 - cia[0]) * (1 - cia[1]) * (1 - cia[2])
    impact = 7.52 * (iss - 0.029) - 3.25 * (iss - 0.02) ** 15 if changed else 6.42 * iss
    exploitability = 8.22 * av * ac * pr * ui
    if impact <= 0:
        score = 0.0
    else:
        raw = min(1.08 * (impact + exploitability), 10) if changed else min(impact + exploitability, 10)
        # CVSS 3.1's Roundup: the smallest one-decimal number not below raw.
        score = math.ceil(round(raw * 100000) / 10000.0) / 10.0
    return "Critical" if score >= 9 else "High" if score >= 7 else "Medium" if score >= 4 else "Low" if score > 0 else "Info"


def osv_scan(dismissed):
    """(findings, outcomes): outcomes is [(library, version, file, status, text)]."""
    findings, outcomes = [], []
    for lib in VENDORED:
        version, source = vendored_version(lib)
        if not version:
            outcomes.append((lib["name"], "?", source, "FAIL", "the version could not be read from %s" % source))
            continue
        if not lib.get("repo"):
            outcomes.append((lib["name"], version, source, "UNKNOWN", lib.get("coverage", "no OSV source")))
            continue
        tag = lib["tag"].format(v=version, v_=version.replace(".", "_"))
        control = lib["tag"].format(v=lib["control"], v_=lib["control"].replace(".", "_"))
        try:
            vulns = osv_query(lib["repo"], tag)
            control_vulns = osv_query(lib["repo"], control)
        except (urllib.error.URLError, OSError, ValueError) as e:
            outcomes.append((lib["name"], version, source, "FAIL", "OSV could not be asked: %s" % e))
            continue
        older = lib.get("oldest_tag") and version_key(version) < version_key(lib["oldest_tag"])
        live = []
        for v in vulns:
            aliases = [v.get("id", "")] + list(v.get("aliases", []) or [])
            hit = next((dismissed[a] for a in aliases if a in dismissed), None)
            fixed = ""
            for affected in v.get("affected", []) or []:
                for r in affected.get("ranges", []) or []:
                    for event in r.get("events", []) or []:
                        if "fixed" in event:
                            fixed = "a later release (fix commit %s)" % event["fixed"][:12]
            record = {
                "scanner": "osv", "scan": "vendored libraries", "file": source, "type": "vendored",
                "package": lib["name"], "version": version, "id": v.get("id", ""),
                "severity": osv_severity(v), "fixed": fixed,
                "title": (v.get("summary") or v.get("details", "").split("\n")[0])[:200],
                "description": (v.get("details") or "")[:2000],
                "url": "https://osv.dev/vulnerability/%s" % v.get("id", ""), "status": "",
            }
            if hit:
                record["dismissed"] = True
                record["status"] = "dismissed in the VEX document (%s)" % hit[1]
            else:
                live.append(record)
            findings.append(record)
        if live:
            outcomes.append((lib["name"], version, source, "FAIL" if any(
                f["severity"] in ("Critical", "High") for f in live) else "FOUND",
                "%d advisor%s: %s" % (len(live), "y" if len(live) == 1 else "ies", ", ".join(f["id"] for f in live))))
        elif older:
            # What affects the oldest version OSV can match most probably affects
            # an older one too: listed for a person to read, not counted.
            try:
                oldest = osv_query(lib["repo"], lib["tag"].format(v=lib["oldest_tag"], v_=lib["oldest_tag"].replace(".", "_")))
            except (urllib.error.URLError, OSError, ValueError):
                oldest = control_vulns
            maybe = sorted({v.get("id") for v in oldest})
            outcomes.append((lib["name"], version, source, "UNKNOWN",
                             "%s is older than the upstream repository's first tag (%s), so OSV cannot match it; "
                             "the advisories OSV has for %s, which probably reach back to it: %s%s" % (
                                 version, lib["oldest_tag"], lib["oldest_tag"], ", ".join(maybe) or "none",
                                 ("; " + lib["coverage"]) if lib.get("coverage") else "")))
        elif not control_vulns:
            outcomes.append((lib["name"], version, source, "UNKNOWN",
                             "OSV names no advisory for %s, but none for the control %s either, which a published "
                             "advisory affects: OSV has no usable data for this library%s" % (
                                 tag, control, (" (%s)" % lib["coverage"]) if lib.get("coverage") else "")))
        else:
            outcomes.append((lib["name"], version, source, "PASS",
                             "no advisory names %s (the control %s draws %d)%s" % (
                                 tag, control, len(control_vulns),
                                 ("; " + lib["coverage"]) if lib.get("coverage") else "")))
    return findings, outcomes


# ---------------------------------------------------------------- the reports

def gitlab_report(scanner, name, version, findings, started, ended, status):
    vulnerabilities = []
    for f in findings:
        if f.get("dismissed"):
            continue
        # A finding's own scanner, which is the report's except in the report of both.
        found_by = f.get("scanner", scanner)
        identifier_type = "cve" if f["id"].startswith("CVE-") else "ghsa" if f["id"].startswith("GHSA-") else found_by
        identifiers = [{"type": identifier_type, "name": f["id"], "value": f["id"]}]
        if f["url"].startswith(("http://", "https://")):
            identifiers[0]["url"] = f["url"]
        item = {
            "id": str(uuid.uuid5(ID_NAMESPACE, "|".join([found_by, f["file"], f["package"], f["version"], f["id"]]))),
            "name": "%s in %s %s" % (f["id"], f["package"], f["version"]),
            "description": f["description"] or f["title"] or f["id"],
            "severity": f["severity"] if f["severity"] in ("Critical", "High", "Medium", "Low", "Info") else "Unknown",
            "identifiers": identifiers,
            "location": {"file": f["file"] or "unknown",
                         "dependency": {"package": {"name": f["package"] or "unknown"}, "version": f["version"] or "unknown"}},
        }
        if f["fixed"]:
            item["solution"] = "Upgrade %s to %s." % (f["package"], f["fixed"])
        if f["url"].startswith(("http://", "https://")):
            item["links"] = [{"url": f["url"]}]
        vulnerabilities.append(item)
    return {
        "version": SCHEMA_VERSION,
        "scan": {
            "analyzer": {"id": "hmailserver-dependency-scan", "name": "hMailServer dependency-scan job",
                         "version": "1", "vendor": {"name": "hMailServer"}},
            "scanner": {"id": scanner, "name": name, "version": version or "unknown", "vendor": {"name": name}},
            "type": "dependency_scanning",
            "start_time": started, "end_time": ended, "status": status,
        },
        "vulnerabilities": vulnerabilities,
    }


def blocking(f):
    return not f.get("dismissed") and f["severity"] in ("Critical", "High") and bool(f["fixed"])


def junit(targets):
    cases = []
    for classname, name, status, text in targets:
        n = quoteattr(name)
        c = quoteattr("dependency-scan." + classname)
        body = escape(text)
        if status == "FAIL":
            cases.append('<testcase classname=%s name=%s><failure message=%s>%s</failure></testcase>' % (
                c, n, quoteattr(text.split("\n")[0][:300]), body))
        elif status == "UNKNOWN":
            cases.append('<testcase classname=%s name=%s><skipped message=%s/></testcase>' % (c, n, quoteattr(text[:500])))
        else:
            cases.append('<testcase classname=%s name=%s><system-out>%s</system-out></testcase>' % (c, n, body))
    failures = sum(1 for t in targets if t[2] == "FAIL")
    skipped = sum(1 for t in targets if t[2] == "UNKNOWN")
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<testsuites>\n<testsuite name="dependency-scan" tests="%d" '
            'failures="%d" skipped="%d">\n%s\n</testsuite>\n</testsuites>\n' % (len(targets), failures, skipped, "\n".join(cases)))


def describe(f):
    fixed = (" fixed in " + f["fixed"]) if f["fixed"] else " (no fixed version)"
    extra = ("  [" + f["status"] + "]") if f.get("dismissed") else ""
    return "%-8s %-22s %s %s in %s%s  - %s%s" % (f["severity"], f["id"], f["package"], f["version"], f["file"],
                                                 fixed, f["title"][:120], extra)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", default=os.path.join("security", "dependency-scan"))
    parser.add_argument("--image-root", default="ci-image-packages")
    parser.add_argument("--skip-trivy", action="store_true", help="the OSV step only (no trivy on PATH)")
    parser.add_argument("--skip-osv", action="store_true")
    args = parser.parse_args()
    os.chdir(ROOT)
    out = args.out
    os.makedirs(out, exist_ok=True)
    log = os.path.join(out, "trivy.log")
    started = now()
    dismissed = vex_not_affected()
    targets, trivy_all, osv_all = [], [], []
    broken = False
    trivy_version = ""

    if not args.skip_trivy:
        if not shutil.which("trivy"):
            print("trivy is not on PATH: this runs in the aquasec/trivy image (or pass --skip-trivy)")
            return 2
        trivy_version = subprocess.run(["trivy", "--version"], capture_output=True, text=True).stdout.split("\n")[0]
        print(trivy_version)
        source = download_database(log)
        if not source:
            print("No vulnerability database from %s: nothing can be scanned." % " or ".join(TRIVY_DB))
            return 2
        print("vulnerability database from %s" % source)
        repo_json = os.path.join(out, "trivy-lockfiles.json")
        code = trivy(["fs", "--skip-dirs", "libraries", "--skip-dirs", "ci-image-packages",
                      "--skip-dirs", "security", "--list-all-pkgs", "."], repo_json, log)
        if code != 0 or not os.path.exists(repo_json):
            broken = True
            targets.append(("trivy", "the lock files", "FAIL", "trivy fs exited %d; see trivy.log" % code))
        else:
            found, packages = trivy_findings(repo_json, "lock files")
            trivy_all += found
            files = sorted({p[0] for p in packages})
            for file in files:
                in_file = [f for f in found if f["file"] == file]
                live = [f for f in in_file if not f.get("dismissed")]
                count = sum(1 for p in packages if p[0] == file)
                text = "%d package(s); %d vulnerabilit%s%s" % (
                    count, len(live), "y" if len(live) == 1 else "ies",
                    (":\n" + "\n".join(describe(f) for f in in_file)) if in_file else "")
                targets.append(("lockfiles", file, "FAIL" if any(blocking(f) for f in live) else "PASS", text))
            # Every lock file that names a package must come back with packages: one
            # Trivy skipped, or could not parse, would otherwise read as clean.
            expected, empty = [], []
            for base, dirs, names in os.walk("."):
                dirs[:] = [d for d in dirs if d not in (".git", "libraries", "ci-image-packages", "security")]
                for n in names:
                    if n in ("packages.lock.json", "packages.config", "requirements.txt"):
                        path = os.path.relpath(os.path.join(base, n), ".").replace("\\", "/")
                        (expected if lock_file_packages(path) else empty).append(path)
            unread = sorted(f for f in expected if f not in files)
            if unread:
                targets.append(("trivy", "every lock file was read", "FAIL",
                                "Trivy reported no packages for: %s" % ", ".join(unread)))
            elif expected:
                targets.append(("trivy", "every lock file was read", "PASS",
                                "%d lock files and manifests, every one with its packages%s" % (
                                    len(expected), ("; %d name no package at all (a project with no NuGet"
                                                    " dependency): %s" % (len(empty), ", ".join(sorted(empty))))
                                    if empty else "")))

        image_root = args.image_root
        if os.path.isdir(image_root):
            image_json = os.path.join(out, "trivy-ci-image.json")
            code = trivy(["rootfs", image_root], image_json, log)
            if code != 0 or not os.path.exists(image_json):
                broken = True
                targets.append(("trivy", "the CI image", "FAIL", "trivy rootfs exited %d; see trivy.log" % code))
            else:
                found, packages = trivy_findings(image_json, "CI image", "ci-image:")
                trivy_all += found
                with open(image_json, encoding="utf-8") as f:
                    os_found = ((json.load(f).get("Metadata") or {}).get("OS") or {})
                os_packages = sum(1 for p in packages if p[1] in ("ubuntu", "debian"))
                live = [f for f in found if not f.get("dismissed")]
                by_severity = {}
                for f in live:
                    by_severity[f["severity"]] = by_severity.get(f["severity"], 0) + 1
                fixable = [f for f in live if blocking(f)]
                text = "%s %s: %d package(s), %d of them the distribution's; %d vulnerabilit%s (%s); %d high or critical with a fixed version%s" % (
                    os_found.get("Family", "?"), os_found.get("Name", "?"),
                    len(packages), os_packages, len(live), "y" if len(live) == 1 else "ies",
                    ", ".join("%s %d" % (k, by_severity[k]) for k in ("Critical", "High", "Medium", "Low", "Unknown") if k in by_severity),
                    len(fixable), (":\n" + "\n".join(describe(f) for f in fixable[:80])) if fixable else "")
                if not os_packages or os_found.get("Family") in (None, "", "none"):
                    # Measured: without etc/lsb-release Trivy detects no OS, says
                    # "Unsupported os" in its log, and reports the dpkg packages as
                    # nothing at all - a clean result that looked at none of them.
                    broken = True
                    targets.append(("image", "hmailserver-ci/linux (dpkg and .NET)", "FAIL",
                                    "Trivy recognised no distribution, or no distribution package, in %s (OS %r): "
                                    "the image's packages were not scanned" % (image_root, os_found)))
                else:
                    targets.append(("image", "hmailserver-ci/linux (dpkg and .NET)",
                                    "FAIL" if fixable else "PASS", text))
        else:
            targets.append(("image", "hmailserver-ci/linux (dpkg and .NET)", "UNKNOWN",
                            "no %s directory: the ci-image-packages job's artifact is not here" % image_root))

    if not args.skip_osv:
        found, outcomes = osv_scan(dismissed)
        osv_all += found
        for name, version, source, status, text in outcomes:
            detail = "\n".join(describe(f) for f in found if f["package"] == name)
            # FOUND: advisories, none high or critical with a fix. Listed, and a pass
            # in the JUnit report, which is SECURITY.md's threshold.
            targets.append(("vendored", "%s %s (%s)" % (name, version, source), status,
                            text + ("\n" + detail if detail else "")))
            if status == "FAIL" and "could not" in text:
                broken = True

    ended = now()
    with open(os.path.join(out, "gl-dependency-scanning-trivy.json"), "w", encoding="utf-8") as f:
        json.dump(gitlab_report("trivy", "Trivy", trivy_version.replace("Version: ", ""), trivy_all, started, ended,
                                "failure" if broken else "success"), f, indent=1)
    with open(os.path.join(out, "gl-dependency-scanning-osv.json"), "w", encoding="utf-8") as f:
        json.dump(gitlab_report("osv", "OSV", "api.osv.dev v1", osv_all, started, ended,
                                "failure" if broken else "success"), f, indent=1)
    # The job's report, both scanners' findings in one file: GitLab takes one per type.
    with open(os.path.join(out, "gl-dependency-scanning.json"), "w", encoding="utf-8") as f:
        json.dump(gitlab_report("trivy-osv", "Trivy and OSV",
                                "Trivy %s; api.osv.dev v1" % trivy_version.replace("Version: ", ""),
                                trivy_all + osv_all, started, ended, "failure" if broken else "success"), f, indent=1)
    with open(os.path.join(out, "junit.xml"), "w", encoding="utf-8") as f:
        f.write(junit(targets))
    order = {"Critical": 0, "High": 1, "Medium": 2, "Low": 3, "Unknown": 4, "Info": 5}
    everything = sorted(trivy_all + osv_all, key=lambda f: (bool(f.get("dismissed")), order.get(f["severity"], 9), f["id"]))
    with open(os.path.join(out, "findings.txt"), "w", encoding="utf-8") as f:
        f.write("".join(describe(x) + "\n" for x in everything))

    print("")
    print("== what was scanned")
    for classname, name, status, text in targets:
        print("%-8s %-10s %s" % (status, classname, name))
        for line in text.split("\n")[:12]:
            print("           " + line)
    live = [f for f in everything if not f.get("dismissed")]
    print("")
    print("%d vulnerabilit%s in all (%d dismissed in the VEX document); %d high or critical with a fixed version." % (
        len(live), "y" if len(live) == 1 else "ies", len(everything) - len(live), sum(1 for f in live if blocking(f))))
    if broken:
        return 2
    return 1 if any(t[2] == "FAIL" for t in targets) else 0


if __name__ == "__main__":
    sys.exit(main())
