#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Known vulnerabilities in what the server is built from: the dependency-scan job.

What replaced GitHub's dependency review (the GitHub workflow dependency-review.yml)
and Dependabot's alerts. Three scans, one report:

  1. The lock files, with Trivy (trivy fs): the twelve NuGet packages.lock.json,
     the three packages.config, and build/ci/requirements.txt, the semgrep job's
     own Python packages. Nothing else in the tree is a manifest: the
     Python tools use the standard library (the three that do not are named by
     repo-hygiene.py), and the Node harnesses install no package.
  2. The CI image, with Trivy (trivy rootfs), from the package database the
     ci-image-packages job copies out of it (Ubuntu's dpkg status and the .NET
     SDK's deps.json files). The Linux server is built in that image and links
     its OpenSSL, libpq, libldap and, statically, its Boost, so this is also the
     Linux packages' native dependencies. An advisory against a header-only
     package of the image (HEADER_ONLY, below: linux-libc-dev, the kernel's
     user-space API headers, under which Trivy files every advisory against the
     kernel's source package) is not affected: it stays in every report with
     that status, the image's summary counts it apart (image_summary), and it
     is never among the high or critical with a fixed version, nor held by the
     release gate.
  3. The libraries the Windows server is built with, which no manifest names:
     OpenSSL, Boost and libpq at the versions hMailServer.vcxproj's include paths
     carry, zlib and QuickJS from their vendored sources, and MariaDB
     Connector/C, 7-Zip and SQL Server Compact from third-party-binaries.json -
     the same places build/native-dependencies.ps1 reads for the SBOM, so the
     version asked about is the version built. Each is asked of OSV
     (api.osv.dev) by its upstream repository's tag. OSV's C and C++ data is
     partial, so each library is also asked about a CONTROL version that a
     recent advisory is known to name: when OSV says nothing about the control
     either, the answer for the real version is UNKNOWN, never clean. And the
     .NET Desktop Runtime each Windows installer carries, at the release
     third-party-binaries.json pins for that installer, asked of OSV by the
     NuGet packages Microsoft publishes its two shared frameworks as -
     Microsoft.WindowsDesktop.App.Runtime and Microsoft.NETCore.App.Runtime,
     win-x64 for the x64 installer and win-arm64 for the ARM64 one - which is
     how OSV's records name the runtime; a control beside each, as for the
     libraries.

Where it comes from - nothing from GitHub. Trivy is Docker Hub's aquasec/trivy,
pinned by digest in .gitlab-ci.yml. Its vulnerability database is an OCI artifact
that Trivy pulls from ghcr.io unless told otherwise; this downloads it from
Google's mirror.gcr.io, or from AWS's public.ecr.aws when that fails, one at a
time and by name (Trivy's own list moves on only after errors it takes for
temporary, and not when a host cannot be reached: measured), and scans with the
download it has. A run that can reach neither fails; it never tries GitHub. OSV is Google's
api.osv.dev. (Both databases carry, among their sources, GitHub's advisory
database; what is avoided is GitHub as a place to download from, not its data.)

Trivy is given no setting of its own beside its command line, and reads no file of the
checkout's that its command line does not name. Each run of it that downloads the
vulnerability database or scans has the job's environment less every variable whose
name begins TRIVY_ - whoever gave the job one: a CI file, in any form of key GitLab's
YAML reader takes, the dotenv report of the job the scan needs, or a CI/CD variable of
the project's or the group's - and, as --config and --ignorefile, an empty
configuration and an empty ignore file this script writes in a directory of its own
outside the checkout, so that a trivy.yaml or a .trivyignore at the root of the
checkout, committed or laid there by an artifact, is not read, and as --cache-dir an
empty directory there, so that the vulnerability database it scans with is the one it
has just downloaded - its default cache is under HOME or XDG_CACHE_HOME, which a
variable could put in the checkout beside a database that says it is fresh, which
--download-db-only would keep -, and as --module-dir another, so that it loads no
module of the checkout's - its default module directory is .trivy/modules under
XDG_DATA_HOME or HOME, every .wasm file in which it loads, and a module that is a
post-scanner may change or drop findings before Trivy writes its report. HOME,
XDG_CACHE_HOME, XDG_CONFIG_HOME and XDG_DATA_HOME name a third empty directory there,
so that no other default Trivy takes from its home is the checkout's either (its VEX
repositories' configuration, Docker's and Podman's registry configuration), and
DOCKER_CONFIG, REGISTRY_AUTH_FILE and XDG_RUNTIME_DIR, which name another place for
the registry configuration, are not given it (own_trivy_files, trivy_environment).
trivy --version, run first for the version the reports name, has the same environment
and the same configuration, cache and home, and neither an ignore file nor a module
directory, which it does not take (installed_trivy): given no --config, it loads the
trivy.yaml of the directory it runs in, the checkout's root. TRIVY_SEVERITY=LOW given
the job so narrowed a real scan to its 114 Low findings, and passed its verdict,
before (16 October 2026). What Trivy is told beside that narrows what the scan reports
by the VEX document alone; the job's other variables reach it as they reach any
program the job runs (PATH among them), and are not held here.

A finding that does not affect the project is dismissed in the VEX document,
.gitlab/hmailserver.openvex.json, as SECURITY.md says: Trivy reads it with --vex,
and the OSV step drops an advisory the document says is not_affected (or fixed) when
the statement names it, by its id or an alias, AND names the library at the version
scanned among its products, by its purl (pkg:generic/<purl>@<version>, the purl
build/native-dependencies.ps1 gives the SBOM): a statement read against zlib 1.3.1
does not dismiss the same advisory for 1.3.2. The document is read, and the products
matched, by build/ci/mr-dependency-check.py, which the change gate uses too.

An advisory's severity is OSV's record's CVSS v3 vector's, else its database's
rating, else its CVSS v4.0 vector's, scored by mr-dependency-check.py (one
implementation of FIRST's v4.0 calculator for both); an OSS-Fuzz crash report (OSV-)
with none of those is Medium, and the rest Unknown.

Writes to --out (default security/dependency-scan): the tools' raw output, a
GitLab dependency-scanning report per scanner (gl-dependency-scanning-trivy.json,
gl-dependency-scanning-osv.json) and one with both (gl-dependency-scanning.json,
the job's report: GitLab takes one file per report type, and refused the upload
of two on the first pipeline, "only one file can be sent as raw"), junit.xml (a
test case per scanned target - the part GitLab's free tier shows),
findings.txt, and verdict.json, the scan's verdict (VERDICT, below): failed when a
high or critical vulnerability with a fixed version is found (SECURITY.md's
threshold: fixed before the next release; one against a header-only package of the
CI image is not affected and is not one) or another target it scanned failed,
broken when a scan could not run, passed otherwise.

Exits 2 when a scan could not run, and 0 when the scans ran, whatever they found:
GitLab takes a job's security report into the vulnerability report only when the
job succeeded - "When a CI/CD job fails, security report results are not ingested
by GitLab, even if the job allows failure" (docs.gitlab.com, "Security scanner
integration", read on 15 October 2026) - and the exit 1 this script gave a finding
before, in a job allowed to fail, filed master's scan of 14 October 2026 (pipeline
2947545170) as JOB_FAILED with none of its findings, which emptied the dependency
side of the vulnerability report. The finding's warning is the
dependency-scan-verdict job's (.gitlab-ci.yml): --verdict reads verdict.json and
exits as the scan job did - 1 for failed, 2 for broken or for a verdict that is
missing, unreadable or another pipeline's, 0 for passed - in a job allowed to fail,
as the scan job is. build/ci/security-gate.py reads the reports themselves,
whatever either job ended with. A retry of the scan job by hand does not run the
verdict job again, since GitLab runs a finished job that needs a retried one again
only when it was skipped: it is retried too, which it says last when it fails
(RETRY_NOTE), and the first line of one that read a verdict names the job that
wrote it.

Usage, from the root of a checkout, in the aquasec/trivy image with python3:

  python3 build/ci/dependency-scan.py [--image-root DIR] [--out DIR] [--skip-trivy] [--skip-osv]
  python3 build/ci/dependency-scan.py --verdict [--out DIR]
  python3 build/ci/dependency-scan.py --self-test

--image-root is the directory the ci-image-packages job left (ci-image-packages/
by default; that scan is skipped, and says so, when it is not there). --self-test
reads no network: the severities, the VEX document's matching, the version
readers, the header-only rule and the CI image's summary, against records and
texts it makes, the tree's own VEX document and pins against each other and against
SECURITY.md's list of the libraries this says UNKNOWN for, and the scan's ending and
its verdict, over a tree it makes with Trivy and its database stood in for.
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
import importlib.util
import json
import math
import re
import shutil
import subprocess
import tempfile
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
# names it (the upstream repository and the tag of a version - or, with an
# ecosystem, the package's name there and the version itself), and a CONTROL
# version that a recent advisory in OSV is known to name, so that "no advisory"
# can be told apart from "no data". control_draws says that OSV did name advisories for
# the control when it was chosen (api.osv.dev on 6 October 2026: OpenSSL 4.0.0 43,
# Boost 1.90.0 2, zlib 1.3 4, MariaDB Connector/C 3.1.7 1, 7-Zip 24.08 2; libpq 17.2
# and QuickJS 2021-03-27 none): for those, a control that draws none is OSV's data not
# read, which fails the scan, never a clean answer. A version older than its control
# that OSV names nothing for, while it names advisories for the control, is one OSV did
# not match (OpenSSL 0.9.6 draws none), and the control's advisories are listed as ones
# that may affect it. oldest_tag is the first version the upstream
# repository has a tag for, where that is later than versions still in use: OSV
# matches a version by its tag, and cannot match one the repository never had.
# ecosystem, when there is one, is OSV's name for the package index whose records name
# the component (NuGet for the .NET runtime, below): OSV is then asked by repo, the
# package's name in it, and the version as it is ("{v}"), never by a repository's tag.
# purl is the name of its pkg:generic purl, as build/native-dependencies.ps1 gives it
# to the SBOM and as a statement of the VEX document names it (--self-test holds the
# two to each other). build/ci/mr-dependency-check.py reads this list, vendored_versions
# and osv_query too: a change that moves one of these versions is asked about the same
# way before it lands.
VENDORED = [
    {"name": "OpenSSL", "purl": "openssl", "file": VCXPROJ, "pattern": r"openssl-(\d+\.\d+\.\d+[a-z]?)",
     "repo": "https://github.com/openssl/openssl", "tag": "openssl-{v}", "control": "4.0.0", "control_draws": True,
     "used": "TLS for every listener and the REST API, DKIM, hashing (Windows build)"},
    {"name": "Boost", "purl": "boost", "file": VCXPROJ, "pattern": r"boost_(\d+)_(\d+)_(\d+)",
     "repo": "https://github.com/boostorg/boost", "tag": "boost-{v}", "control": "1.90.0", "control_draws": True,
     "used": "Asio under all socket I/O, threads, filesystem, regex (Windows build)",
     "coverage": "OSV's Boost records are OSS-Fuzz crash reports only; Boost's CVEs are not among them"},
    {"name": "libpq (PostgreSQL client)", "purl": "postgresql", "file": VCXPROJ, "pattern": r"postgresql-(\d+)\.(\d+)",
     "repo": "https://github.com/postgres/postgres", "tag": "REL_{v_}", "control": "17.2",
     "used": "the PostgreSQL backend's client library (Windows build)"},
    {"name": "zlib", "purl": "zlib", "file": "hmailserver/source/Server/zlib/zlib.h", "pattern": r'#define ZLIB_VERSION "([0-9.]+)"',
     "repo": "https://github.com/madler/zlib", "tag": "v{v}", "control": "1.3", "control_draws": True,
     "used": "vendored source: IMAP COMPRESS=DEFLATE and the message store's compression (Windows build)"},
    {"name": "QuickJS", "purl": "quickjs", "file": "hmailserver/source/Server/quickjs/VERSION", "pattern": r"^\s*(\d{4}-\d{2}-\d{2})\s*$",
     "repo": "https://github.com/bellard/quickjs", "tag": "{v}", "control": "2021-03-27",
     "used": "vendored source: the event scripts' engine on the Linux builds"},
    {"name": "MariaDB Connector/C", "purl": "mariadb-connector-c", "file": BINARIES, "component": "MariaDB Connector/C",
     "repo": "https://github.com/mariadb-corporation/mariadb-connector-c", "tag": "v{v}", "control": "3.1.7", "control_draws": True,
     "used": "libmysql.dll, the MySQL and MariaDB backends' client, shipped by the installer"},
    {"name": "7-Zip (7za.exe)", "purl": "7-zip", "file": BINARIES, "component": "7-Zip standalone console (7za.exe)",
     "repo": "https://github.com/ip7z/7zip", "tag": "{v}", "control": "24.08", "control_draws": True, "oldest_tag": "22.01",
     "used": "creates and restores backups (Compression.cpp), shipped by the installer",
     "coverage": "OSV's 7-Zip data is partial: CVE-2023-31102 (a crafted 7z archive and the PPMd decoder) "
                 "is in OSV only with a range it could not resolve to the repository, so no query by version finds it"},
    {"name": "SQL Server Compact 4.0", "purl": "sql-server-compact", "file": BINARIES,
     "component": "Microsoft SQL Server Compact 4.0 SP1 x64 ENU",
     "repo": None, "used": "the built-in database, shipped by the installer",
     "coverage": "no open vulnerability database tracks it; Microsoft ended its support in July 2021"},
]

# The .NET Desktop Runtime each Windows installer carries and installs where no .NET 10
# Desktop Runtime is present: third-party-binaries.json's two downloaded entries, one
# for each installer, each pinning one release of Microsoft's. One release's installer
# holds two shared frameworks of that release, Microsoft.WindowsDesktop.App and
# Microsoft.NETCore.App, which Microsoft publishes on NuGet as the runtime packages
# below, one per platform, and OSV's records name the runtime's advisories under those
# packages. So each package is asked of OSV in the NuGet ecosystem at the release its
# installer's entry pins, with the control 10.0.12 beside it, a release OSV names
# advisories for (it did for each of the four packages on 14 October 2026), so that its
# silence is OSV's data not read. The SBOM names both installers' runtime
# pkg:generic/dotnet-windowsdesktop-runtime (the ARM64 one with ?arch=arm64), so a VEX
# statement names it so, at the release.
DOTNET_RUNTIME_INSTALLERS = (
    ("Microsoft .NET Windows Desktop Runtime (x64 installer)", "win-x64", "the x64 installer"),
    ("Microsoft .NET Windows Desktop Runtime (ARM64 installer)", "win-arm64", "the ARM64 installer"),
)
DOTNET_RUNTIME_PACKAGES = (
    ("Microsoft.WindowsDesktop.App.Runtime", "the Windows Desktop runtime, which the Control Panel runs on"),
    ("Microsoft.NETCore.App.Runtime", "the .NET runtime, which the Control Panel and the database tools run on"),
)
VENDORED += [
    {"name": "%s.%s" % (package, rid), "purl": "dotnet-windowsdesktop-runtime", "file": BINARIES, "component": component,
     "ecosystem": "NuGet", "repo": "%s.%s" % (package, rid), "tag": "{v}", "control": "10.0.12", "control_draws": True,
     "used": "%s, in the .NET Desktop Runtime %s carries and installs where no .NET 10 Desktop Runtime is present"
             % (what, installer)}
    for component, rid, installer in DOTNET_RUNTIME_INSTALLERS for package, what in DOTNET_RUNTIME_PACKAGES]


_mr_check = []


def mr_check():
    """build/ci/mr-dependency-check.py, loaded the first time it is asked for: the CVSS
    v4.0 score and the reading of the VEX document are its, so that the change gate and
    this scan rate and dismiss an advisory alike. Not at import, so that it may load this
    file in turn."""
    if not _mr_check:
        spec = importlib.util.spec_from_file_location("mr_dependency_check",
                                                      os.path.join(ROOT, "build", "ci", "mr-dependency-check.py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _mr_check.append(module)
    return _mr_check[0]


def now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as f:
        return f.read()


def version_key(v):
    return [int(x) if x.isdigit() else x for x in re.split(r"[.\-]", v)]


# ---------------------------------------------------------------- the VEX document

def vex_not_affected():
    """The not_affected and fixed statements of the VEX document, as
    mr-dependency-check.py's vex_statements reads them: each matched by an advisory's id
    or alias and by the library's purl at its version (vex_dismissal). A document that
    cannot be read dismisses nothing, and says so: what it would have dismissed is then
    counted, which holds a release rather than letting one through."""
    try:
        return mr_check().vex_statements(read(VEX))
    except (OSError, ValueError) as e:
        print("The VEX document %s cannot be read (%s): it dismisses nothing in this scan." % (VEX.replace(os.sep, "/"), e))
        return []


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


# What Trivy reads beside its command line, by which what it reports would narrow past
# what this script asks of it - and so what dependency-scan-verdict fails on: a variable
# of its own (TRIVY_SEVERITY, TRIVY_SKIP_DIRS, TRIVY_IGNOREFILE ...), whoever gave the
# job one - a CI file in any of the forms GitLab's YAML reader takes for a key, a rule,
# the dotenv report of the job the scan needs, a project or group CI/CD variable -, and
# at the root of the checkout, where it runs, its configuration file and its ignore file
# (--config's and --ignorefile's defaults, trivy.yaml and .trivyignore), which the tree,
# an artifact laid over the checkout or a cache could put there. So every run of Trivy
# that downloads the database or scans has the job's environment less every variable
# whose name begins TRIVY_, in any case (trivy_environment), and --config and
# --ignorefile naming two files of this script's, an empty configuration and an empty
# ignore file, in a directory made for the run in the system's temporary directory,
# outside the checkout and the scan's artifacts, and removed when the scan ends
# (own_trivy_files, forget_trivy_files). Its cache, where it keeps the vulnerability
# database, is an empty directory there too (--cache-dir): the default is under HOME or
# XDG_CACHE_HOME, which a variable could put in the checkout beside a database that says
# it is fresh, which trivy --download-db-only keeps. So is its module directory
# (--module-dir), whose default, .trivy/modules under XDG_DATA_HOME or HOME, could be
# put in the checkout the same way: Trivy loads every .wasm file in it when
# --enable-modules names none, as here, and a module that is a post-scanner may change
# or drop the scan's findings before Trivy writes its report. And since every other
# default Trivy takes from its home could be put there too - among them its VEX
# repositories' configuration and Docker's and Podman's registry configuration, which
# can name a credential helper for it to run -, each run has HOME and the XDG base
# directories it reads (TRIVY_HOME_VARIABLES) naming an empty directory of this script's
# beside the files, and none of the variables that name another place for the registry
# configuration (TRIVY_REGISTRY_VARIABLES): the vulnerability database is public, and
# asks no login. trivy --version, which takes neither --ignorefile nor --module-dir
# (Trivy 0.74.0 refuses both there as unknown flags), is given the same environment,
# configuration and cache (installed_trivy), since with no --config it loads the
# trivy.yaml of the directory it runs in, the checkout's root.
TRIVY_VARIABLE_PREFIX = "TRIVY_"
TRIVY_HOME_VARIABLES = ("HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME")
TRIVY_REGISTRY_VARIABLES = ("DOCKER_CONFIG", "REGISTRY_AUTH_FILE", "XDG_RUNTIME_DIR")
_own_trivy_files = []


def trivy_environment(environ=None):
    """The environment Trivy runs with: environ (the job's) less every variable whose
    name begins TRIVY_ and every one TRIVY_HOME_VARIABLES or TRIVY_REGISTRY_VARIABLES
    names, in any case, and with each of TRIVY_HOME_VARIABLES naming own_trivy_files'
    home."""
    environ = os.environ if environ is None else environ
    home = own_trivy_files()[4]
    dropped = TRIVY_HOME_VARIABLES + TRIVY_REGISTRY_VARIABLES
    given = dict((name, value) for name, value in environ.items()
                 if not name.upper().startswith(TRIVY_VARIABLE_PREFIX) and name.upper() not in dropped)
    given.update((name, home) for name in TRIVY_HOME_VARIABLES)
    return given


def own_trivy_files():
    """(configuration, ignore file, cache, modules, home): the two files and the three
    directories this script gives Trivy as --config, --ignorefile, --cache-dir and
    --module-dir and as its home (trivy_environment), an empty mapping, an empty file and
    three empty directories, made the first time they are asked for in a directory made
    for the run in the system's temporary directory."""
    if not _own_trivy_files:
        held = tempfile.mkdtemp(prefix="dependency-scan-trivy-")
        config, ignore = os.path.join(held, "trivy.yaml"), os.path.join(held, "trivyignore")
        cache, modules, home = (os.path.join(held, name) for name in ("cache", "modules", "home"))
        with open(config, "w", encoding="utf-8") as f:
            f.write("{}\n")
        with open(ignore, "w", encoding="utf-8"):
            pass
        for directory in (cache, modules, home):
            os.mkdir(directory)
        _own_trivy_files[:] = [held, config, ignore, cache, modules, home]
    return tuple(_own_trivy_files[1:])


def forget_trivy_files():
    """Removes own_trivy_files' directory, when there is one."""
    if _own_trivy_files:
        shutil.rmtree(_own_trivy_files[0], ignore_errors=True)
        del _own_trivy_files[:]


def run_trivy(command, log):
    """command, a run of Trivy, with own_trivy_files as its --config, --ignorefile,
    --cache-dir and --module-dir and trivy_environment as its environment, from the root
    of the checkout, its output added to log: its exit code."""
    config, ignore, cache, modules, home = own_trivy_files()
    command = command + ["--config", config, "--ignorefile", ignore, "--cache-dir", cache, "--module-dir", modules]
    print("$ " + " ".join(command))
    with open(log, "a", encoding="utf-8") as out:
        return subprocess.run(command, cwd=ROOT, env=trivy_environment(), stdout=out, stderr=subprocess.STDOUT).returncode


def installed_trivy():
    """The first line of trivy --version, or None when there is no trivy on PATH. It runs
    as run_trivy runs Trivy - from the root of the checkout, with trivy_environment, and
    with own_trivy_files' configuration and cache as its --config and --cache-dir, so that
    it reads neither the checkout's trivy.yaml, which it loads as --config's default when
    given no --config, nor the metadata of a database the checkout holds - but for
    --ignorefile and --module-dir, which trivy --version does not take (Trivy 0.74.0
    refuses both there as unknown flags)."""
    if not shutil.which("trivy"):
        return None
    files = own_trivy_files()
    command = ["trivy", "--version", "--config", files[0], "--cache-dir", files[2]]
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                          env=trivy_environment()).stdout.split("\n")[0]


def download_database(log):
    """The repository the vulnerability database came from, or None when none answered."""
    for repository in TRIVY_DB:
        command = ["trivy", "image", "--download-db-only", "--db-repository", repository,
                   "--skip-version-check", "--disable-telemetry", "--no-progress"]
        if run_trivy(command, log) == 0:
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
    return run_trivy(command, log)


def trivy_findings(path, label, file_prefix=""):
    """(findings, packages) from a Trivy JSON report file."""
    with open(path, encoding="utf-8") as f:
        return report_findings(json.load(f), label, file_prefix)


def report_findings(report, label, file_prefix=""):
    """(findings, packages) from a Trivy report, loaded. Each finding's file is the
    result's target behind file_prefix (IMAGE_PREFIX for the CI image's scan), and one
    against a header-only package of the CI image carries not_affected, HEADER_ONLY's
    status (header_only)."""
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
    for f in findings:
        if header_only(f["package"], f["file"]):
            f["not_affected"] = HEADER_ONLY_STATUS
    return findings, packages


# ---------------------------------------------------------------- OSV, for the vendored libraries

def osv_query(repo, tag, ecosystem="GIT"):
    """Every advisory api.osv.dev names for a version: by a repository's tag (ecosystem
    GIT, the default), or by a package's name and version in another ecosystem
    (VENDORED's ecosystem, NuGet for the .NET runtime)."""
    body = json.dumps({"package": {"name": repo, "ecosystem": ecosystem}, "version": tag}).encode("utf-8")
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


def vendored_versions(lib, text):
    """Every version of lib that text - the file VENDORED names for it - pins, first one
    first and each once: the project file names OpenSSL, Boost and libpq once for each
    platform and configuration, and third-party-binaries.json lists each file of a
    component. ValueError when the file is third-party-binaries.json and not JSON."""
    found = []
    if "component" in lib:
        data = json.loads(text.lstrip("﻿"))
        values = [str(a.get("version", "")).split()[0] for a in data.get("artifacts", [])
                  if isinstance(a, dict) and a.get("component") == lib["component"] and str(a.get("version", "")).split()]
    else:
        values = [".".join(m.groups()) for m in re.finditer(lib["pattern"], text, re.M)]
    for value in values:
        if value not in found:
            found.append(value)
    return found


def vendored_version(lib):
    """(the version the release is scanned for, the file it is read from): the first
    the file pins, or None."""
    try:
        versions = vendored_versions(lib, read(lib["file"]))
    except ValueError:
        versions = []
    return (versions[0] if versions else None), lib["file"]


def advisory_fix(v, lib, version):
    """What fixes advisory v for lib at version, as the report's solution names it, or ""
    when OSV's record names no fix. A library asked by its repository's tag (GIT): a
    later release, with the fix commit the record gives. One asked by its package in an
    ecosystem (NuGet): the release that ends the record's range holding the version, for
    that package - the 10.0 range's fix for a 10.0 runtime, where the record has an 8.0
    and a 9.0 range beside it - or, when no range holds it, the fixes the record names
    for the package."""
    ecosystem = lib.get("ecosystem", "GIT")
    if ecosystem == "GIT":
        fixed = ""
        for affected in v.get("affected", []) or []:
            for r in affected.get("ranges", []) or []:
                for event in r.get("events", []) or []:
                    if "fixed" in event:
                        fixed = "a later release (fix commit %s)" % event["fixed"][:12]
        return fixed

    def order(release):
        # Numbers compared as numbers, and a word (a pre-release's) after any number, so
        # that two versions always compare.
        return [(0, p) if isinstance(p, int) else (1, p) for p in version_key(release)]
    holding, named = [], []
    for affected in v.get("affected", []) or []:
        package = affected.get("package") or {}
        if str(package.get("ecosystem", "")).lower() != ecosystem.lower() or \
                str(package.get("name", "")).lower() != lib["repo"].lower():
            continue
        for r in affected.get("ranges", []) or []:
            introduced = "0"
            for event in r.get("events", []) or []:
                if "introduced" in event:
                    introduced = str(event["introduced"])
                elif "fixed" in event:
                    fixed = str(event["fixed"])
                    named.append(fixed)
                    if order(introduced) <= order(version) < order(fixed):
                        holding.append(fixed)
    if holding:
        return min(holding, key=order)
    named = sorted(set(named), key=order)
    return ("a later release (%s)" % ", ".join(named)) if named else ""


def osv_severity(v):
    for s in v.get("severity", []) or []:
        score = str(s.get("score", ""))
        if score.startswith("CVSS:3."):
            return cvss3_rating(score)
    rating = (v.get("database_specific") or {}).get("severity")
    if isinstance(rating, str) and rating.title() in ("Critical", "High", "Medium", "Low"):
        return rating.title()
    # A record rated by a CVSS v4.0 vector alone, as the CVE records OSV converts from
    # CVE's own list since 2026 often are (CVE-2026-76844 in zlib's gz_write, 8.3).
    for s in v.get("severity", []) or []:
        if s.get("type") == "CVSS_V4" or str(s.get("score", "")).startswith("CVSS:4.0/"):
            score = mr_check().cvss4_score(str(s.get("score", "")))
            if score is not None:
                return mr_check().rating_of_score(score) or "Info"
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


def without_usable_data(lib, version):
    """Whether OSV holds no usable data on lib at version, as osv_scan finds it: no
    repository OSV knows (UNKNOWN whatever OSV says), a control OSV was not known to name
    when it was chosen (no control_draws: UNKNOWN while the control draws nothing), or a
    version older than the upstream repository's first tag, which OSV cannot match.
    SECURITY.md ("Dependencies") names these as what the scan says UNKNOWN for today,
    and --self-test holds it to them."""
    return not lib.get("repo") or not lib.get("control_draws") or bool(
        lib.get("oldest_tag") and version and version_key(version) < version_key(lib["oldest_tag"]))


def osv_scan(dismissed):
    """(findings, outcomes): outcomes is [(library, version, file, status, text)].
    dismissed is vex_not_affected()'s statements."""
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
        ecosystem = lib.get("ecosystem", "GIT")
        try:
            vulns = osv_query(lib["repo"], tag, ecosystem)
            control_vulns = osv_query(lib["repo"], control, ecosystem)
        except (urllib.error.URLError, OSError, ValueError) as e:
            outcomes.append((lib["name"], version, source, "FAIL", "OSV could not be asked: %s" % e))
            continue
        if lib.get("control_draws") and not control_vulns:
            outcomes.append((lib["name"], version, source, "FAIL",
                             "OSV could not be read for %s: it names no advisory for the control %s, which it is known to "
                             "name, so its answer for %s cannot be taken" % (lib["name"], control, tag)))
            continue
        older = lib.get("oldest_tag") and version_key(version) < version_key(lib["oldest_tag"])
        below_control = not vulns and control_vulns and version_key(version) < version_key(lib["control"])
        live = []
        for v in vulns:
            aliases = [v.get("id", "")] + list(v.get("aliases", []) or [])
            # The statement must name this library at this version, not the advisory alone.
            hit = mr_check().vex_dismissal(dismissed, aliases, "generic", lib["purl"], version)
            fixed = advisory_fix(v, lib, version)
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
                record["status"] = "dismissed in the VEX document (%s)" % hit
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
                oldest = osv_query(lib["repo"], lib["tag"].format(v=lib["oldest_tag"], v_=lib["oldest_tag"].replace(".", "_")),
                                   ecosystem)
            except (urllib.error.URLError, OSError, ValueError):
                oldest = control_vulns
            maybe = sorted({v.get("id") for v in oldest})
            outcomes.append((lib["name"], version, source, "UNKNOWN",
                             "%s is older than the upstream repository's first tag (%s), so OSV cannot match it; "
                             "the advisories OSV has for %s, which probably reach back to it: %s%s" % (
                                 version, lib["oldest_tag"], lib["oldest_tag"], ", ".join(maybe) or "none",
                                 ("; " + lib["coverage"]) if lib.get("coverage") else "")))
        elif below_control:
            # Older than the control, named in no advisory where the control is: OSV did
            # not match its tag, and what affects the control probably reaches back.
            outcomes.append((lib["name"], version, source, "UNKNOWN",
                             "OSV names no advisory for %s, but %d for the control %s, which is newer: OSV did not match "
                             "the older tag, and the control's advisories probably affect it too: %s%s" % (
                                 tag, len(control_vulns), control, ", ".join(sorted({v.get("id") for v in control_vulns})),
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
        if f.get("not_affected"):
            # The record GitLab's dashboard shows carries the status, in the report
            # schema's details (a named text field): its id, name, severity and solution
            # are those of any other finding, so it is the record the dashboard already
            # holds, and nothing of it is hidden.
            item["details"] = {"not_affected": {"name": "Not affected", "type": "text",
                                                "value": "%s (%s)" % (f["not_affected"], HEADER_ONLY.get(f["package"], ""))}}
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


# ---------------------------------------------------------------- the header-only rule
#
# HEADER_ONLY: the packages of the CI image that install headers alone - files under
# /usr/include, no program and no library - built from a source package whose
# advisories Trivy files under every binary package built from it. Ubuntu's
# linux-libc-dev is the one: the kernel's user-space API headers (/usr/include/linux,
# /usr/include/asm-generic and the rest), built from the kernel's source package, and
# every advisory against the kernel is filed under it - 3,535 of the image's 3,608 on
# 6 October 2026, and on 8 October, Ubuntu having published 7.0.0-38.38 while the image
# held 7.0.0-34.34, 60 of them High or Critical with a fixed version, which would have
# held the next release on headers that hold no kernel code; the image holds no kernel
# at all, since its jobs run on their runner's. So an advisory against a package named
# here, found by the CI image's scan, is not affected (HEADER_ONLY_STATUS): it stays in
# every report with that status - findings.txt, the JUnit text, and the record of the
# GitLab report, whose details carry it, so that the dashboard shows it too - and the
# image's summary counts it apart; but it is never among the high or critical with a
# fixed version (blocking), and build/ci/security-gate.py, which applies this same rule
# (header_only) to the report it reads, never holds a release on it. The rule reaches
# the packages named here and no other - not linux-libc-dev-arm64-cross, not libc6 -
# and the CI image's scan alone (IMAGE_PREFIX): the same name in a lock file counts as
# any package. SECURITY.md, "Dependencies"; the maintainers dismissed the 3,535 of 6
# October on the dashboard as not applicable with this reason.
HEADER_ONLY = {
    "linux-libc-dev": "the kernel's user-space API headers, built from the kernel's source package, whose advisories "
                      "Trivy files under it",
}
HEADER_ONLY_STATUS = "not affected: header-only package, the advisory is against the kernel it was built from"
# What the CI image's findings' file begins with (trivy_findings' file_prefix): the
# GitLab report's location.file keeps it, and the gate reads the rule off it.
IMAGE_PREFIX = "ci-image:"


def header_only(package, file):
    """Whether HEADER_ONLY's rule reaches an advisory against package, found at file - a
    finding's file here, a record's location.file in the GitLab report: a package named
    in HEADER_ONLY, in the CI image's scan, and nothing else."""
    return (file or "").startswith(IMAGE_PREFIX) and package in HEADER_ONLY


def blocking(f):
    """Whether a finding holds the release (SECURITY.md's threshold): not dismissed in
    the VEX document, not a header-only package's, Critical or High, with a fixed version."""
    return (not f.get("dismissed") and not header_only(f["package"], f["file"])
            and f["severity"] in ("Critical", "High") and bool(f["fixed"]))


SEVERITY_ORDER = ("Critical", "High", "Medium", "Low", "Unknown")


def severities(findings):
    """'Critical 1, High 2, ...' for the findings, or 'none'."""
    counted = {}
    for f in findings:
        counted[f["severity"]] = counted.get(f["severity"], 0) + 1
    order = list(SEVERITY_ORDER) + sorted(k for k in counted if k not in SEVERITY_ORDER)
    return ", ".join("%s %d" % (k, counted[k]) for k in order if k in counted) or "none"


def image_summary(os_found, packages, live, fixable):
    """The CI image's test case text: its packages, its vulnerabilities by severity, those
    against its header-only packages (HEADER_ONLY: not affected, and how many of them
    would otherwise have held the release) and those against its other packages apart,
    and the ones that hold the release, listed."""
    os_packages = sum(1 for p in packages if p[1] in ("ubuntu", "debian"))
    headers = [f for f in live if header_only(f["package"], f["file"])]
    others = [f for f in live if not header_only(f["package"], f["file"])]
    names = sorted({f["package"] for f in headers}) or sorted(HEADER_ONLY)
    spared = [f for f in headers if f["severity"] in ("Critical", "High") and f["fixed"]]
    return ("%s %s: %d package(s), %d of them the distribution's; %d vulnerabilit%s (%s), %d of them against %s (%s) - "
            "%s - %s, %d of them high or critical with a fixed version, which hold nothing; and %d against the image's "
            "other packages (%s); %d high or critical with a fixed version%s" % (
                os_found.get("Family", "?"), os_found.get("Name", "?"), len(packages), os_packages,
                len(live), "y" if len(live) == 1 else "ies", severities(live), len(headers), ", ".join(names),
                severities(headers), "; ".join(HEADER_ONLY[n] for n in names), HEADER_ONLY_STATUS, len(spared),
                len(others), severities(others), len(fixable),
                (":\n" + "\n".join(describe(f) for f in fixable[:80])) if fixable else ""))


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
    status = f["status"] if f.get("dismissed") else f.get("not_affected", "")
    extra = ("  [" + status + "]") if status else ""
    return "%-8s %-22s %s %s in %s%s  - %s%s" % (f["severity"], f["id"], f["package"], f["version"], f["file"],
                                                 fixed, f["title"][:120], extra)


# ---------------------------------------------------------------- the verdict
#
# What the scan came to, beside its reports (VERDICT, in --out), for the
# dependency-scan-verdict job, which fails on it as the scan job itself used to (the
# module's docstring says why the scan job no longer does): failed, exit 1, when a
# target the scans read failed - a Critical or High vulnerability with a fixed version
# that the VEX document does not dismiss, which holds the next release unless an
# exception accepts it, or a lock file Trivy did not read -; broken, exit 2, when a scan
# could not run, which the scan job still says by failing itself; passed, exit 0,
# otherwise. It names the pipeline and the job it was written in, and read in a
# pipeline, a verdict that names another pipeline, or none, stands for nothing: a
# verdict.json committed to the tree, or kept from another run, cannot pass the job in
# place of this pipeline's.
VERDICT = "verdict.json"
VERDICT_EXIT = {"passed": 0, "failed": 1, "broken": 2}
# How many lines of a failed target's text the job's log gives; junit.xml and
# findings.txt hold every one.
VERDICT_LINES = 40
# What the job says last when it fails. A retry of dependency-scan by hand does not run
# it again: GitLab, when a job is retried, runs again only the jobs after it that were
# skipped (Ci::ResetSkippedJobsService), and this one ran - it is not skipped when the
# scan fails, since the scan is allowed to fail - so its answer stays the one it gave
# the attempt before, whose job its first line names ("written by job N").
RETRY_NOTE = ("If dependency-scan has been retried by hand since this job ran, this is the answer to the attempt before: "
              "GitLab runs a finished job that needs a retried one again only when it was skipped, so retry this job too.")


def write_verdict(out, verdict, says, failed=(), blocking_count=0, environ=None):
    """Writes VERDICT into out, and returns what it wrote: the verdict (one of
    VERDICT_EXIT's) and the exit it calls for, what the scan said of what it found, each
    target that failed with its text, how many findings hold the release (blocking), and
    the pipeline, the job and the commit it was written in, by GitLab's variables (none
    outside a pipeline)."""
    environ = os.environ if environ is None else environ
    record = {"scan": "dependency-scan", "verdict": verdict, "exit": VERDICT_EXIT[verdict], "says": says,
              "failed": [{"target": "%s %s" % (classname, name), "text": text} for classname, name, _, text in failed],
              "blocking": blocking_count, "pipeline": environ.get("CI_PIPELINE_ID", ""),
              "job": environ.get("CI_JOB_ID", ""), "commit": environ.get("CI_COMMIT_SHA", ""), "written": now()}
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, VERDICT), "w", encoding="utf-8") as f:
        json.dump(record, f, indent=1)
    return record


def verdict_job(out, environ, say=print):
    """The dependency-scan-verdict job: read_verdict's answer, and RETRY_NOTE after it when
    it fails."""
    code = read_verdict(out, environ, say)
    if code:
        say(RETRY_NOTE)
    return code


def read_verdict(out, environ, say=print):
    """The verdict the scan wrote into out (VERDICT), said, and the exit it calls for - 2
    when there is none, when what is there is not one, and, read in a pipeline, when
    another pipeline wrote it."""
    shown = os.path.join(out, VERDICT).replace(os.sep, "/")
    unread = "so nothing says that dependency-scan's scans ran"
    try:
        with open(os.path.join(out, VERDICT), encoding="utf-8") as f:
            record = json.load(f)
    except OSError:
        say("FAILED: there is no %s: dependency-scan stopped before its verdict, or its artifacts did not reach this job "
            "(its log says which), %s." % (shown, unread))
        return 2
    except ValueError as e:
        say("FAILED: %s is not JSON (%s), %s." % (shown, e, unread))
        return 2
    verdict = record.get("verdict") if isinstance(record, dict) else None
    if verdict not in VERDICT_EXIT or record.get("exit") != VERDICT_EXIT[verdict]:
        say("FAILED: %s holds no verdict this reads (%r), %s." % (shown, verdict, unread))
        return 2
    pipeline = environ.get("CI_PIPELINE_ID", "")
    if pipeline and str(record.get("pipeline") or "") != pipeline:
        say("FAILED: %s was written in pipeline %s, not in this one (%s): a verdict committed to the tree, or kept from "
            "another run, stands for nothing, %s." % (shown, record.get("pipeline") or "none", pipeline, unread))
        return 2
    say("dependency-scan's verdict: %s (%s, written by job %s of pipeline %s at %s)." % (
        verdict, shown, record.get("job") or "none", record.get("pipeline") or "none", record.get("written") or "?"))
    failed = [t for t in record.get("failed") or [] if isinstance(t, dict)]
    for target in failed:
        say("FAIL     %s" % target.get("target", "?"))
        lines = str(target.get("text") or "").split("\n")
        for line in lines[:VERDICT_LINES]:
            say("           " + line)
        if len(lines) > VERDICT_LINES:
            say("           ... and %d line(s) more, which junit.xml and findings.txt hold" % (len(lines) - VERDICT_LINES))
    if record.get("says"):
        say(str(record["says"]))
    held = record.get("blocking") or 0
    if verdict == "failed":
        say("FAILED: %d of the targets dependency-scan read failed%s. This job is allowed to fail: the finding warns and "
            "holds no change, as dependency-scan's own failure on it did, and dependency-scan passed, so that GitLab "
            "takes its findings into the vulnerability report." % (
                len(failed), (", with %d Critical or High vulnerabilit%s with a fixed version among their findings that the "
                              "VEX document does not dismiss: each holds the next release unless "
                              ".gitlab/security-exceptions.yml accepts it (SECURITY.md, \"Before a release\")" % (
                                  held, "y" if held == 1 else "ies")) if held else ""))
    elif verdict == "broken":
        say("FAILED: a scan of dependency-scan could not run, and its job failed with it: its reports say nothing is clean, "
            "the release's gate holds on them, and GitLab took none of their findings.")
    else:
        say("No target dependency-scan read failed.")
    return VERDICT_EXIT[verdict]


# ---------------------------------------------------------------- the self-test

def self_test():
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    # Severity: a CVSS v3 vector first, as before; the database's rating; a CVSS v4.0
    # vector, scored by mr-dependency-check.py; an OSS-Fuzz crash; nothing.
    def record(id, vector=None, kind=None, label=None):
        v = {"id": id}
        if vector:
            v["severity"] = [{"type": kind or ("CVSS_V4" if vector.startswith("CVSS:4.0/") else "CVSS_V3"), "score": vector}]
        if label:
            v["database_specific"] = {"severity": label}
        return v
    gz_write = "CVSS:4.0/AV:N/AC:H/AT:N/PR:N/UI:N/VC:N/VI:H/VA:H/SC:N/SI:N/SA:N"
    for v, rating, what in (
            (record("CVE-2026-76844", gz_write), "High", "a record rated by a CVSS v4.0 vector alone (CVE-2026-76844, 8.3)"),
            (record("CVE-1", "CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N"), "Critical", "a v4.0 9.3"),
            (record("CVE-2", "CVSS:4.0/AV:N/AC:H/AT:P/PR:L/UI:A/VC:L/VI:N/VA:N/SC:N/SI:N/SA:N"), "Low", "a v4.0 2.0"),
            (record("CVE-3", "CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H"), "Critical", "a CVSS v3 9.8"),
            (record("CVE-4", "CVSS:3.1/AV:N/AC:L/PR:N/UI:R/S:C/C:L/I:L/A:N", label="HIGH"), "Medium",
             "a CVSS v3 vector before the database's rating, as before"),
            (record("CVE-5", gz_write, label="LOW"), "Low", "the database's rating before a CVSS v4.0 vector"),
            (record("OSV-2026-1"), "Medium", "an OSS-Fuzz crash with no rating"),
            (record("OSV-2026-2", gz_write), "High", "an OSS-Fuzz crash with a CVSS v4.0 vector"),
            (record("CVE-6", "CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N"), "Unknown",
             "a v4.0 vector without every base metric"),
            (record("CVE-7"), "Unknown", "a record with no rating at all")):
        got = osv_severity(v)
        expect(got == rating, "%s is %s, not %s" % (what, rating, got))

    # The VEX document: an advisory is dismissed for the library at the version a
    # statement names, by its id or an alias - not for another version or library.
    document = json.dumps({"@context": "https://openvex.dev/ns/v0.2.0", "statements": [
        {"vulnerability": {"name": "CVE-2026-0001", "aliases": ["GHSA-aaaa-bbbb-cccc"]},
         "products": [{"@id": "pkg:generic/zlib@1.3.2", "identifiers": {"purl": "pkg:generic/zlib@1.3.2"}}],
         "status": "not_affected", "justification": "vulnerable_code_not_present"},
        {"vulnerability": {"name": "CVE-2026-0002"}, "products": [{"@id": "pkg:generic/zlib"}],
         "status": "not_affected", "justification": "vulnerable_code_not_present"},
        {"vulnerability": {"name": "CVE-2026-0003"}, "products": [{"@id": "pkg:generic/boost@1.92.0"}],
         "status": "affected"}]})
    statements = mr_check().vex_statements(document)
    dismissal = mr_check().vex_dismissal
    expect(dismissal(statements, ["CVE-2026-0001"], "generic", "zlib", "1.3.2") == "vulnerable_code_not_present",
           "a statement for zlib 1.3.2 dismisses its advisory for zlib 1.3.2")
    expect(dismissal(statements, ["OSV-X", "GHSA-aaaa-bbbb-cccc"], "generic", "zlib", "1.3.2"),
           "and by the advisory's alias")
    expect(dismissal(statements, ["CVE-2026-0001"], "generic", "zlib", "1.3.1") is None,
           "a statement for zlib 1.3.2 does not dismiss the advisory for zlib 1.3.1")
    expect(dismissal(statements, ["CVE-2026-0001"], "generic", "boost", "1.3.2") is None,
           "nor for another library at the same version")
    expect(dismissal(statements, ["CVE-2026-0002"], "generic", "zlib", "1.3.2") is None,
           "a product with no version dismisses nothing: a statement names the version it was read against")
    expect(dismissal(statements, ["CVE-2026-0003"], "generic", "boost", "1.92.0") is None,
           "a statement that says affected dismisses nothing")

    # The header-only rule: it reaches the packages HEADER_ONLY names, by name and not by
    # a prefix, in the CI image's scan and in no other; an advisory under it is not
    # affected and never blocking, the GitLab report's record keeps its severity and
    # solution and carries the status in its details, and the report's id for it is
    # the one the dashboard already holds; a High with a fixed version against any
    # other package of the image, or against the same name in a lock file, still counts.
    KERNEL = "linux-libc-dev"
    image_file = IMAGE_PREFIX + "ubuntu"
    expect(KERNEL in HEADER_ONLY and HEADER_ONLY_STATUS.startswith("not affected: header-only package"),
           "HEADER_ONLY names linux-libc-dev, and its status says not affected and why")
    expect(header_only(KERNEL, image_file) and not header_only(KERNEL + "-arm64-cross", image_file)
           and not header_only("libc6", image_file) and not header_only(KERNEL, "hmailserver/x/packages.lock.json")
           and not header_only(KERNEL, "") and not header_only(KERNEL, None),
           "the rule reaches the named package in the CI image's scan, and nothing else")
    trivy_report = {"Results": [{"Target": "ubuntu", "Type": "ubuntu", "Class": "os-pkgs",
                                 "Packages": [{"Name": KERNEL, "Version": "7.0.0-34.34"}, {"Name": "libc6", "Version": "2.43"}],
                                 "Vulnerabilities": [
                                     {"PkgName": KERNEL, "InstalledVersion": "7.0.0-34.34", "VulnerabilityID": "CVE-2026-1",
                                      "Severity": "HIGH", "FixedVersion": "7.0.0-38.38", "Title": "kernel", "PrimaryURL": "https://x/1"},
                                     {"PkgName": KERNEL, "InstalledVersion": "7.0.0-34.34", "VulnerabilityID": "CVE-2026-2",
                                      "Severity": "CRITICAL", "FixedVersion": "7.0.0-38.38", "Title": "kernel"},
                                     {"PkgName": "libc6", "InstalledVersion": "2.43", "VulnerabilityID": "CVE-2026-3",
                                      "Severity": "HIGH", "FixedVersion": "2.43-2ubuntu2.4", "Title": "libc"}]}]}
    image_found, image_packages = report_findings(trivy_report, "CI image", IMAGE_PREFIX)
    lock_found, _ = report_findings(trivy_report, "lock files")
    kernel_found = [f for f in image_found if f["package"] == KERNEL]
    libc_found = [f for f in image_found if f["package"] == "libc6"]
    expect(len(kernel_found) == 2 and all(f.get("not_affected") == HEADER_ONLY_STATUS and not blocking(f) for f in kernel_found)
           and all(f["file"] == image_file and f["severity"] in ("High", "Critical") and f["fixed"] for f in kernel_found),
           "a Critical or High with a fixed version against linux-libc-dev in the CI image is not affected and holds nothing: %s"
           % kernel_found)
    expect(len(libc_found) == 1 and not libc_found[0].get("not_affected") and blocking(libc_found[0]),
           "a High with a fixed version against any other package of the image still counts: %s" % libc_found)
    expect(len(lock_found) == 3 and all(not f.get("not_affected") and blocking(f) for f in lock_found),
           "the rule is the CI image's scan's alone: the same name in a lock file counts as any package: %s" % lock_found)
    expect(describe(kernel_found[0]).endswith("  [" + HEADER_ONLY_STATUS + "]") and not describe(libc_found[0]).endswith("]"),
           "findings.txt says the status on the finding's line: %s" % describe(kernel_found[0]))
    report = gitlab_report("trivy", "Trivy", "1", image_found, "2026-10-08T00:00:00", "2026-10-08T00:00:01", "success")
    records = dict((v["identifiers"][0]["value"], v) for v in report["vulnerabilities"])
    expect(sorted(records) == ["CVE-2026-1", "CVE-2026-2", "CVE-2026-3"]
           and all(records[i]["severity"] == s and records[i]["solution"] for i, s in
                   (("CVE-2026-1", "High"), ("CVE-2026-2", "Critical"), ("CVE-2026-3", "High")))
           and all(records[i]["details"]["not_affected"]["type"] == "text"
                   and records[i]["details"]["not_affected"]["value"].startswith(HEADER_ONLY_STATUS) for i in ("CVE-2026-1", "CVE-2026-2"))
           and "details" not in records["CVE-2026-3"],
           "the GitLab report keeps every record, its severity and its solution, and the header-only ones carry the status "
           "in their details: %s" % json.dumps(report["vulnerabilities"])[:600])
    expect(records["CVE-2026-1"]["id"] == str(uuid.uuid5(ID_NAMESPACE, "|".join(["trivy", image_file, KERNEL, "7.0.0-34.34", "CVE-2026-1"])))
           and records["CVE-2026-1"]["location"] == {"file": image_file, "dependency": {"package": {"name": KERNEL}, "version": "7.0.0-34.34"}},
           "the record's id and location are the ones the dashboard already holds, so the rule hides nothing and moves nothing")
    # The CI image's summary: the header-only packages' advisories counted apart from the
    # other packages', by name and not by a prefix, with how many would have held the
    # release, every one still counted in the whole, and the ones that do hold it listed.
    def image_finding(package, severity, fixed="", file=image_file):
        f = {"scanner": "trivy", "scan": "CI image", "file": file, "type": "ubuntu", "package": package,
             "version": "1", "id": "CVE-2026-1", "severity": severity, "fixed": fixed, "title": "t",
             "description": "", "url": "", "status": ""}
        if header_only(package, file):
            f["not_affected"] = HEADER_ONLY_STATUS
        return f
    image_packages = [(image_file, "ubuntu", KERNEL, "7.0.0-34.34"), (image_file, "ubuntu", "libc6", "2.43"),
                      (IMAGE_PREFIX + "usr/share/dotnet", "dotnet-core", "System.Text.Json", "10.0.0")]
    image_live = ([image_finding(KERNEL, "Medium")] * 3 + [image_finding(KERNEL, "High")] +
                  [image_finding("libc6", "Medium"), image_finding("patch", "Low"),
                   image_finding(KERNEL + "-arm64-cross", "Low")])
    summary = image_summary({"Family": "ubuntu", "Name": "26.04"}, image_packages, image_live, [])
    expect(summary == ("ubuntu 26.04: 3 package(s), 2 of them the distribution's; 7 vulnerabilities (High 1, Medium 4, "
                       "Low 2), 4 of them against linux-libc-dev (High 1, Medium 3) - the kernel's user-space API headers, "
                       "built from the kernel's source package, whose advisories Trivy files under it - not affected: "
                       "header-only package, the advisory is against the kernel it was built from, 0 of them high or "
                       "critical with a fixed version, which hold nothing; and 3 against the image's other packages "
                       "(Medium 1, Low 2); 0 high or critical with a fixed version"),
           "the CI image's summary counts linux-libc-dev's advisories apart, and only that package's: %s" % summary)
    spared = image_finding(KERNEL, "High", "7.0.0-38.38")
    held = image_finding("libc6", "High", "2.43-2ubuntu2.4")
    fixable = [f for f in image_live + [spared, held] if blocking(f)]
    summary = image_summary({"Family": "ubuntu", "Name": "26.04"}, image_packages, image_live + [spared, held], fixable)
    expect(fixable == [held] and "9 vulnerabilities (High 3, Medium 4, Low 2), 5 of them against linux-libc-dev (High 2, Medium 3)" in summary
           and ", 1 of them high or critical with a fixed version, which hold nothing; and 4 against the image's other packages "
               "(High 1, Medium 1, Low 2); 1 high or critical with a fixed version:\n" + describe(held) in summary
           and describe(spared) not in summary,
           "a fixable High against linux-libc-dev is counted among the headers' and holds nothing, while one against libc6 "
           "is listed as holding the release: %s" % summary)
    summary = image_summary({"Family": "ubuntu", "Name": "26.04"}, image_packages, [], [])
    expect("0 vulnerabilities (none), 0 of them against linux-libc-dev (none)" in summary
           and ", 0 of them high or critical with a fixed version, which hold nothing; and 0 against the image's other "
               "packages (none); 0 high or critical with a fixed version" in summary,
           "an image with no advisory says so in each count: %s" % summary)

    # The versions as each file pins them.
    libraries = dict((lib["purl"], lib) for lib in VENDORED)
    project = ("<I>$(hMailServerLibs)\\boost_1_92_0;$(hMailServerLibs)\\openssl-4.0.3\\out64\\include;"
               "$(hMailServerLibs)\\postgresql-18.6\\src</I>\n<I>$(hMailServerLibs)\\openssl-4.0.3-arm64\\out64\\include</I>\n"
               "<L>$(hMailServerLibs)\\openssl-4.0.2-arm64\\out64\\lib</L>\n")
    expect(vendored_versions(libraries["openssl"], project) == ["4.0.3", "4.0.2"],
           "every OpenSSL the project file names, first first: %s" % vendored_versions(libraries["openssl"], project))
    expect(vendored_versions(libraries["boost"], project) == ["1.92.0"], "Boost from boost_1_92_0")
    expect(vendored_versions(libraries["postgresql"], project) == ["18.6"], "libpq from postgresql-18.6")
    expect(vendored_versions(libraries["zlib"], '#define ZLIB_VERSION "1.3.2"\n#define ZLIB_VERNUM 0x1320\n') == ["1.3.2"],
           "zlib from zlib.h")
    expect(vendored_versions(libraries["quickjs"], "2026-06-04\n") == ["2026-06-04"], "QuickJS from its VERSION")
    manifest = json.dumps({"artifacts": [{"component": "MariaDB Connector/C", "version": "3.4.9"},
                                         {"component": "MariaDB Connector/C", "version": "3.4.9"},
                                         {"component": "7-Zip standalone console (7za.exe)", "version": "26.03"},
                                         {"component": "MariaDB Connector/C", "version": "3.4.10 (x86)"}]})
    expect(vendored_versions(libraries["mariadb-connector-c"], manifest) == ["3.4.9", "3.4.10"],
           "each version of a component's files, once")
    # The .NET runtime's packages, each at the release its own installer's entry pins.
    runtime = dict((lib["name"], lib) for lib in VENDORED if lib.get("ecosystem") == "NuGet")
    runtime_manifest = json.dumps({"artifacts": [
        {"component": "Microsoft .NET Windows Desktop Runtime (x64 installer)", "version": "10.0.13", "disposition": "downloaded"},
        {"component": "Microsoft .NET Windows Desktop Runtime (ARM64 installer)", "version": "10.0.14", "platform": "arm64",
         "disposition": "downloaded"}]})
    for name, release in (("Microsoft.WindowsDesktop.App.Runtime.win-x64", "10.0.13"),
                          ("Microsoft.NETCore.App.Runtime.win-x64", "10.0.13"),
                          ("Microsoft.WindowsDesktop.App.Runtime.win-arm64", "10.0.14"),
                          ("Microsoft.NETCore.App.Runtime.win-arm64", "10.0.14")):
        lib = runtime.get(name)
        expect(lib is not None and lib["repo"] == name and lib["tag"] == "{v}" and vendored_versions(lib, runtime_manifest) == [release],
               "VENDORED asks OSV about the NuGet package %s at the release its installer's entry pins (%s): %s"
               % (name, release, lib and vendored_versions(lib, runtime_manifest)))

    # Where OSV's record says a runtime advisory is fixed: the release that ends the
    # range holding the version, for the package asked about. (A library of its own
    # here, so that what is held below is the scan's reading, whatever VENDORED holds.)
    desktop = {"name": "Microsoft.WindowsDesktop.App.Runtime.win-x64", "purl": "dotnet-windowsdesktop-runtime", "file": BINARIES,
               "component": "Microsoft .NET Windows Desktop Runtime (x64 installer)", "ecosystem": "NuGet",
               "repo": "Microsoft.WindowsDesktop.App.Runtime.win-x64", "tag": "{v}", "control": "10.0.12", "control_draws": True}

    def nuget_range(package, introduced, fixed):
        return {"package": {"ecosystem": "NuGet", "name": package},
                "ranges": [{"type": "ECOSYSTEM", "events": [{"introduced": introduced}, {"fixed": fixed}]}]}
    runtime_record = {"id": "GHSA-0000-0000-0001", "aliases": ["CVE-0000-00001"], "summary": "an advisory of the runtime",
                      "database_specific": {"severity": "HIGH"},
                      "affected": [nuget_range(desktop["repo"], "8.0.0", "8.0.32"), nuget_range(desktop["repo"], "10.0.0", "10.0.13"),
                                   nuget_range("Microsoft.WindowsDesktop.App.Runtime.win-x86", "10.0.0", "10.0.99")]}
    for version, fix in (("10.0.12", "10.0.13"), ("10.0.0", "10.0.13"), ("8.0.5", "8.0.32"),
                         ("10.0.13", "a later release (8.0.32, 10.0.13)")):
        got = advisory_fix(runtime_record, desktop, version)
        expect(got == fix, "a NuGet record's fix for %s %s is %r, not %r" % (desktop["repo"], version, fix, got))
    git_record = {"id": "CVE-0000-00002", "affected": [{"ranges": [{"type": "GIT", "repo": "r",
                                                                     "events": [{"introduced": "0"}, {"fixed": "f" * 40}]}]}]}
    expect(advisory_fix(git_record, libraries["zlib"], "1.3.2") == "a later release (fix commit ffffffffffff)",
           "a repository's record's fix is a later release and its fix commit, as before")

    # What OSV answers for a library: a control OSV is known to name that draws nothing
    # is its data not read, which fails the scan; and a version older than a control that
    # draws advisories, named in none itself, is one OSV did not match.
    module = globals()
    held = (module["osv_query"], module["vendored_version"], list(VENDORED))
    try:
        answers, asked = {}, []

        def fake_query(repo, tag, ecosystem="GIT"):
            asked.append((ecosystem, repo, tag))
            return [dict(a) for a in answers.get((ecosystem, repo, tag) if ecosystem != "GIT" else (repo, tag), [])]
        zlib = dict(libraries["zlib"])
        module["osv_query"] = fake_query
        VENDORED[:] = [zlib]
        module["vendored_version"] = lambda lib: ("1.2.0", "zlib.h")
        _, outcomes = osv_scan([])
        expect(len(outcomes) == 1 and outcomes[0][3] == "FAIL" and "could not" in outcomes[0][4],
               "a control OSV is known to name that draws no advisory fails the scan: %s" % outcomes)
        answers[(zlib["repo"], "v1.3")] = [{"id": "CVE-2026-27171"}]
        _, outcomes = osv_scan([])
        expect(len(outcomes) == 1 and outcomes[0][3] == "UNKNOWN" and "CVE-2026-27171" in outcomes[0][4],
               "a version below the control that OSV names nothing for, where it names the control's, is UNKNOWN with "
               "the control's advisories: %s" % outcomes)
        module["vendored_version"] = lambda lib: ("1.3.2", "zlib.h")
        _, outcomes = osv_scan([])
        expect(len(outcomes) == 1 and outcomes[0][3] == "PASS", "a version after the control that OSV names nothing for "
               "passes, the control drawing its advisories: %s" % outcomes)
        quickjs = dict(libraries["quickjs"])
        VENDORED[:] = [quickjs]
        module["vendored_version"] = lambda lib: ("2026-06-04", "VERSION")
        _, outcomes = osv_scan([])
        expect(len(outcomes) == 1 and outcomes[0][3] == "UNKNOWN", "a control OSV is not known to name, drawing nothing, "
               "leaves the library UNKNOWN: %s" % outcomes)

        # The .NET runtime: asked in OSV's NuGet ecosystem by its package and the pinned
        # release, never by a repository's tag. A High advisory whose range for the
        # package holds the release fails the scan, with the release that ends the range
        # as the fix the report gives - so the release gate holds it; a release past the
        # range passes, the control drawing the advisory; and a control that draws nothing
        # is OSV's data not read.
        VENDORED[:] = [dict(desktop)]
        answers.clear()
        answers[("NuGet", desktop["repo"], "10.0.12")] = [runtime_record]
        del asked[:]
        module["vendored_version"] = lambda lib: ("10.0.12", BINARIES)
        found, outcomes = osv_scan([])
        expect(asked == [("NuGet", desktop["repo"], "10.0.12"), ("NuGet", desktop["repo"], "10.0.12")],
               "the runtime's package is asked of OSV in the NuGet ecosystem, at the release and at its control: %s" % asked)
        expect(len(outcomes) == 1 and outcomes[0][3] == "FAIL" and "GHSA-0000-0000-0001" in outcomes[0][4] and
               "could not" not in outcomes[0][4], "a pinned runtime a High advisory names fails the scan: %s" % outcomes)
        expect(len(found) == 1 and found[0]["severity"] == "High" and found[0]["fixed"] == "10.0.13" and blocking(found[0])
               and found[0]["package"] == desktop["repo"] and found[0]["file"] == BINARIES,
               "and it is a High with a fixed version, against the package, at the manifest: %s" % found)
        report = gitlab_report("osv", "OSV", "api.osv.dev v1", found, "2026-10-14T00:00:00", "2026-10-14T00:00:01", "success")
        expect([(v["severity"], v.get("solution"), v["location"]["dependency"]["package"]["name"]) for v in report["vulnerabilities"]]
               == [("High", "Upgrade %s to 10.0.13." % desktop["repo"], desktop["repo"])],
               "the GitLab report's record is High with the fixed release as its solution, which the release gate holds: %s"
               % report["vulnerabilities"])
        module["vendored_version"] = lambda lib: ("10.0.13", BINARIES)
        found, outcomes = osv_scan([])
        expect(len(outcomes) == 1 and outcomes[0][3] == "PASS" and not found,
               "a pinned runtime past the advisory's range passes, the control drawing it: %s" % outcomes)
        answers.clear()
        found, outcomes = osv_scan([])
        expect(len(outcomes) == 1 and outcomes[0][3] == "FAIL" and "could not" in outcomes[0][4],
               "a runtime control that draws nothing is OSV's data not read, which fails the scan: %s" % outcomes)
    finally:
        module["osv_query"], module["vendored_version"] = held[0], held[1]
        VENDORED[:] = held[2]

    # The tree: each runtime the installers carry - every downloaded entry of
    # third-party-binaries.json - is asked of OSV by both of its NuGet packages, for its
    # platform, at the release the entry pins.
    try:
        text = read(BINARIES)
        artifacts = json.loads(text[1:] if text.startswith(chr(0xFEFF)) else text).get("artifacts", [])
    except (OSError, ValueError) as e:
        artifacts = []
        failures.append("the tree's %s is read: %s" % (BINARIES, e))
    downloaded = [a for a in artifacts if isinstance(a, dict) and a.get("disposition") == "downloaded"]
    expect(len(downloaded) == 2, "the tree's %s pins a runtime for each installer: %s" % (BINARIES, [a.get("path") for a in downloaded]))
    for artifact in downloaded:
        rid = "win-arm64" if artifact.get("platform") == "arm64" else "win-x64"
        reading = [lib for lib in VENDORED if lib.get("file") == BINARIES and lib.get("component") == artifact.get("component")]
        expect(sorted((lib.get("ecosystem"), lib.get("repo")) for lib in reading) ==
               [("NuGet", "Microsoft.NETCore.App.Runtime." + rid), ("NuGet", "Microsoft.WindowsDesktop.App.Runtime." + rid)]
               and all(vendored_version(lib)[0] == str(artifact.get("version")) for lib in reading),
               "the runtime %s carries (%s) is asked of OSV by Microsoft.WindowsDesktop.App.Runtime.%s and "
               "Microsoft.NETCore.App.Runtime.%s at %s: %s" % (artifact.get("path"), artifact.get("component"), rid, rid,
                                                              artifact.get("version"), [lib.get("name") for lib in reading]))

    # The tree: every library has its purl, the SBOM's; the version each file pins is
    # read; and every statement of the VEX document parses and names, among its
    # generic products, only libraries of this list.
    sbom = read(os.path.join("build", "native-dependencies.ps1"))
    sbom_names = set(re.findall(r"'pkg:generic/([a-z0-9.+\-]+)@", sbom))
    for lib in VENDORED:
        expect(lib.get("purl") in sbom_names, "%s's purl, pkg:generic/%s, is the one build/native-dependencies.ps1 gives "
               "the SBOM (%s)" % (lib["name"], lib.get("purl"), ", ".join(sorted(sbom_names))))
        expect(vendored_version(lib)[0], "the tree's %s pins a version of %s that is read" % (lib["file"], lib["name"]))
    try:
        tree = mr_check().vex_statements(read(VEX))
    except (OSError, ValueError) as e:
        tree = []
        failures.append("the tree's VEX document is read: %s" % e)
    expect(tree, "the tree's VEX document has statements that dismiss")
    for names, products, _ in tree:
        for kind, name, version in products:
            if kind == "generic":
                expect(name in libraries and version, "the VEX document's statement for %s names %s@%s, which is no "
                       "library of VENDORED at a version" % (", ".join(sorted(names)), name, version))

    # The scan's ending and its verdict: main() over a tree made here - a manifest and the
    # CI image's package database -, with Trivy and its database stood in for and OSV left
    # out. A High with a fixed version against the image's libc6 ends 0, with every report
    # written and the finding in GitLab's, and the verdict failed, on which --verdict ends
    # 1; the same scan finding nothing ends 0, and passed, on which it ends 0; a scan with
    # no database, or whose trivy fs failed, ends 2, and broken, on which it ends 2 - as
    # the scan job itself ended before it passed whatever its scans found, so that GitLab
    # takes its report (VERDICT).
    # And --verdict takes none for one: no verdict, one that is not JSON, names no verdict
    # or an exit that is not its verdict's, and, read in a pipeline, another pipeline's.
    # Failing, it says last that after a retry of dependency-scan by hand it is retried
    # too (RETRY_NOTE); passing, it says nothing of a retry.
    import contextlib
    import io
    import tempfile
    scratch = tempfile.mkdtemp(prefix="dependency-scan-self-test-")
    kept = (module["ROOT"], module["installed_trivy"], module["download_database"], module["trivy"], os.getcwd())
    image_vulnerabilities, failing = [], []

    def fake_trivy(args, output, log):
        if args[0] in failing:
            return 1
        if args[0] == "fs":
            answer = {"Results": [{"Target": "build/ci/requirements.txt", "Type": "pip",
                                   "Packages": [{"Name": "requests", "Version": "2.32.5"}]}]}
        else:
            answer = {"Metadata": {"OS": {"Family": "ubuntu", "Name": "26.04"}},
                      "Results": [{"Target": "ubuntu", "Type": "ubuntu", "Packages": [{"Name": "libc6", "Version": "2.43"}],
                                   "Vulnerabilities": list(image_vulnerabilities)}]}
        with open(output, "w", encoding="utf-8") as f:
            json.dump(answer, f)
        return 0

    def run(*argv):
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            code = main(list(argv))
        return code, said.getvalue()

    def in_out(out, name):
        try:
            with open(os.path.join(out, name), encoding="utf-8") as f:
                return f.read()
        except OSError:
            return None

    try:
        tree = os.path.join(scratch, "tree")
        for path, text in ((os.path.join("build", "ci", "requirements.txt"), "requests==2.32.5\n"),
                           (VEX, json.dumps({"@context": "https://openvex.dev/ns/v0.2.0", "statements": []})),
                           (os.path.join("ci-image-packages", "etc", "lsb-release"), "DISTRIB_ID=Ubuntu\n")):
            os.makedirs(os.path.dirname(os.path.join(tree, path)), exist_ok=True)
            with open(os.path.join(tree, path), "w", encoding="utf-8") as f:
                f.write(text)
        mr_check()  # from the tree's own build/ci, before ROOT names the one made here
        module["ROOT"] = tree
        module["installed_trivy"] = lambda: "Version: 0.74.0"
        module["download_database"] = lambda log: TRIVY_DB[0]
        module["trivy"] = fake_trivy
        reports = ("gl-dependency-scanning.json", "gl-dependency-scanning-trivy.json", "gl-dependency-scanning-osv.json",
                   "junit.xml", "findings.txt")

        image_vulnerabilities[:] = [{"PkgName": "libc6", "InstalledVersion": "2.43", "VulnerabilityID": "CVE-2026-3",
                                     "Severity": "HIGH", "FixedVersion": "2.43-2ubuntu2.4", "Title": "libc"}]
        out = os.path.join(scratch, "failed")
        code, said = run("--skip-osv", "--out", out)
        record = json.loads(in_out(out, VERDICT) or "{}")
        report = json.loads(in_out(out, "gl-dependency-scanning.json") or "{}")
        expect(code == 0 and all(in_out(out, name) is not None for name in reports),
               "a scan that finds a High with a fixed version ends 0, every report written: %d, %s" % (
                   code, [name for name in reports if in_out(out, name) is None]))
        expect((report.get("scan") or {}).get("status") == "success"
               and [(v["identifiers"][0]["value"], v["severity"], bool(v.get("solution")))
                    for v in report.get("vulnerabilities", [])] == [("CVE-2026-3", "High", True)],
               "the finding is in the job's GitLab report, of a scan that succeeded: %s" % json.dumps(report)[:400])
        expect(record.get("verdict") == "failed" and record.get("exit") == 1 and record.get("blocking") == 1
               and [t["target"] for t in record.get("failed", [])] == ["image hmailserver-ci/linux (dpkg and .NET)"]
               and "CVE-2026-3" in record["failed"][0]["text"],
               "its verdict says it failed, on the image's High with a fixed version: %s" % record)
        code, said = run("--verdict", "--out", out)
        expect(code == 1 and "FAILED: 1 of the targets dependency-scan read failed, with 1 Critical or High "
               "vulnerability with a fixed version" in said and "CVE-2026-3" in said,
               "--verdict ends 1 on it, naming the target and the finding: %d, %s" % (code, said[-600:]))
        expect(said.rstrip("\n").split("\n")[-1] == RETRY_NOTE,
               "--verdict, failing, says last that after a retry of dependency-scan by hand this job is retried too, "
               "which GitLab does not do: %s" % said[-300:])

        image_vulnerabilities[:] = []
        out = os.path.join(scratch, "passed")
        code, said = run("--skip-osv", "--out", out)
        record = json.loads(in_out(out, VERDICT) or "{}")
        expect(code == 0 and all(in_out(out, name) is not None for name in reports)
               and record.get("verdict") == "passed" and record.get("exit") == 0 and record.get("failed") == [],
               "a scan that finds nothing ends 0, every report written, and its verdict says it passed: %d, %s" % (
                   code, record))
        code, said = run("--verdict", "--out", out)
        expect(code == 0 and "No target dependency-scan read failed." in said,
               "--verdict ends 0 on it: %d, %s" % (code, said[-300:]))
        expect(RETRY_NOTE not in said, "--verdict, passing, says nothing of a retry: %s" % said[-300:])

        module["download_database"] = lambda log: None
        out = os.path.join(scratch, "broken")
        code, said = run("--skip-osv", "--out", out)
        record = json.loads(in_out(out, VERDICT) or "{}")
        expect(code == 2 and record.get("verdict") == "broken" and record.get("exit") == 2
               and in_out(out, "gl-dependency-scanning.json") is None,
               "a scan with no vulnerability database ends 2, and its verdict says it is broken: %d, %s" % (code, record))
        code, said = run("--verdict", "--out", out)
        expect(code == 2 and "could not run" in said, "--verdict ends 2 on it: %d, %s" % (code, said[-300:]))
        expect(said.rstrip("\n").split("\n")[-1] == RETRY_NOTE,
               "--verdict, failing on a scan that could not run, says last that after a retry of dependency-scan by hand "
               "this job is retried too: %s" % said[-300:])
        # And one that could not run part of the way, its reports written as a failure.
        module["download_database"] = lambda log: TRIVY_DB[0]
        failing[:] = ["fs"]
        out = os.path.join(scratch, "broken-later")
        code, said = run("--skip-osv", "--out", out)
        record = json.loads(in_out(out, VERDICT) or "{}")
        report = json.loads(in_out(out, "gl-dependency-scanning.json") or "{}")
        expect(code == 2 and record.get("verdict") == "broken" and (report.get("scan") or {}).get("status") == "failure",
               "a scan whose trivy fs failed ends 2, its report says so, and its verdict says it is broken: %d, %s, %s" % (
                   code, record, report.get("scan")))
        code, said = run("--verdict", "--out", out)
        expect(code == 2, "--verdict ends 2 on it: %d, %s" % (code, said[-300:]))
        failing[:] = []

        # Trivy given nothing of its own beside what this script asks of it
        # (TRIVY_VARIABLE_PREFIX): main() over the tree made here with the real
        # download_database() and trivy(), and subprocess.run stood in for, answering as
        # Trivy would, in a job whose environment holds variables of Trivy's and whose
        # checkout holds a trivy.yaml and a .trivyignore at its root - TRIVY_SEVERITY=LOW,
        # given the job in a form of key the checker did not read or by a dotenv report,
        # narrowed a real scan to its 114 Low findings and passed its verdict (check 2 of
        # cycle 3 of G5's fix, 16 October 2026). Each run of Trivy that downloads the
        # database or scans has the job's environment less those variables, and names as
        # --config and --ignorefile, once each, the same empty configuration and empty
        # ignore file, and as --cache-dir the same empty directory beside them, outside
        # the checkout, gone once the scan has ended; and the scan ends as before. The
        # job's HOME and XDG base directories name the checkout, which holds a .wasm file
        # in .trivy/modules and a Docker configuration, and DOCKER_CONFIG,
        # REGISTRY_AUTH_FILE and XDG_RUNTIME_DIR name places in it too - Trivy loads every
        # module in --module-dir's default, under HOME or XDG_DATA_HOME, and a post-scan
        # one may drop findings (check 3 of cycle 3, 16 October 2026): each run is given
        # --module-dir once, an empty directory beside the configuration, as its modules,
        # HOME and each XDG base directory naming one more empty directory there, and none
        # of the three. Which of the job's variables a run must be without, and which must
        # name its home, is read from the names planted here, written out (home_names,
        # withheld), and never from the lists the script reads them by
        # (TRIVY_VARIABLE_PREFIX, TRIVY_HOME_VARIABLES, TRIVY_REGISTRY_VARIABLES), so that
        # a name taken out of one of those lists fails here rather than shortening the
        # case with it (check 4 of cycle 7, 16 October 2026). And the real
        # installed_trivy() runs too, with shutil.which stood in to find a trivy:
        # trivy --version, run with no --config, loads the trivy.yaml of the directory it
        # runs in, the checkout's root (check 4 of cycle 7, with Trivy 0.74.0), so it is
        # given the same configuration, cache and home as the other runs, and neither
        # --ignorefile nor --module-dir, which it refuses as unknown flags.
        module["download_database"], module["trivy"] = kept[2], kept[3]
        image_vulnerabilities[:] = [{"PkgName": "libc6", "InstalledVersion": "2.43", "VulnerabilityID": "CVE-2026-3",
                                     "Severity": "HIGH", "FixedVersion": "2.43-2ubuntu2.4", "Title": "libc"}]
        planted_files = (("trivy.yaml", "severity:\n  - LOW\n"), (".trivyignore", "CVE-2026-3\n"),
                         (os.path.join(".trivy", "modules", "planted.wasm"), "\0asm\1\0\0\0"),
                         (os.path.join(".docker", "config.json"), '{"credsStore": "checkout"}\n'))
        for name, text in planted_files:
            os.makedirs(os.path.dirname(os.path.join(tree, name)), exist_ok=True)
            with open(os.path.join(tree, name), "w", encoding="utf-8") as f:
                f.write(text)
        calls = []
        home_names = ("HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME")

        def listing(path):
            return sorted(os.listdir(path)) if path and os.path.isdir(path) else None

        def stand_in(command, **options):
            seen = {"command": list(command), "env": options.get("env"), "files": {}}
            for flag in ("--config", "--ignorefile", "--cache-dir", "--module-dir"):
                if flag in command[:-1]:
                    path = command[command.index(flag) + 1]
                    if os.path.isdir(path):
                        seen["files"][flag] = (path, listing(path) if flag in ("--cache-dir", "--module-dir") else None)
                        continue
                    try:
                        with open(path, encoding="utf-8") as f:
                            seen["files"][flag] = (path, f.read())
                    except OSError:
                        seen["files"][flag] = (path, None)
            environment = seen["env"] if seen["env"] is not None else os.environ
            seen["home"] = dict((name, (environment.get(name), listing(environment.get(name))))
                                for name in home_names)
            calls.append(seen)
            if command[1:2] == ["--version"]:
                return subprocess.CompletedProcess(command, 0, "Version: 0.74.0\n", "")
            if "--output" in command[:-1]:
                fake_trivy(command[1:2], command[command.index("--output") + 1], None)
            return subprocess.CompletedProcess(command, 0, "", "")

        planted = {"TRIVY_SEVERITY": "LOW", "TRIVY_IGNOREFILE": os.path.join(tree, ".trivyignore"),
                   "Trivy_Skip_Dirs": "ci-image-packages", "HM_DEPENDENCY_SCAN_SELF_TEST": "kept",
                   "HOME": tree, "XDG_CACHE_HOME": tree, "XDG_CONFIG_HOME": tree, "XDG_DATA_HOME": tree,
                   "DOCKER_CONFIG": os.path.join(tree, ".docker"), "REGISTRY_AUTH_FILE": os.path.join(tree, "auth.json"),
                   "XDG_RUNTIME_DIR": tree}
        # Every planted name but the one the job's own and the four of the home, in any
        # case: Trivy's three and the three that name another place for the registry
        # configuration.
        withheld = sorted(set(name.upper() for name in planted) - set(("HM_DEPENDENCY_SCAN_SELF_TEST",) + home_names))
        expect(withheld == ["DOCKER_CONFIG", "REGISTRY_AUTH_FILE", "TRIVY_IGNOREFILE", "TRIVY_SEVERITY",
                            "TRIVY_SKIP_DIRS", "XDG_RUNTIME_DIR"],
               "the case plants Trivy's three variables and the three registry ones: %s" % withheld)
        environ_held = dict((name, os.environ.get(name)) for name in planted)
        run_held, which_held = subprocess.run, shutil.which
        try:
            os.environ.update(planted)
            subprocess.run = stand_in
            shutil.which = lambda name, *more, **options: (
                os.path.join(scratch, "trivy") if name == "trivy" else which_held(name, *more, **options))
            module["installed_trivy"] = kept[1]
            out = os.path.join(scratch, "own-files")
            code, said = run("--skip-osv", "--out", out)
        finally:
            subprocess.run, shutil.which = run_held, which_held
            module["installed_trivy"] = lambda: "Version: 0.74.0"
            for name, value in environ_held.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value
        record = json.loads(in_out(out, VERDICT) or "{}")
        kinds = [call["command"][1] for call in calls if len(call["command"]) > 1]
        expect(code == 0 and record.get("verdict") == "failed" and kinds == ["--version", "image", "fs", "rootfs"],
               "the scan with Trivy's command stood in for runs it four times, for its version, the database, the lock "
               "files and the CI image, and ends 0 with the image's High failing its verdict, as before: %d, %s, %s" % (
                   code, kinds, record.get("verdict")))
        scanner = (json.loads(in_out(out, "gl-dependency-scanning-trivy.json") or "{}").get("scan") or {}).get("scanner")
        expect((scanner or {}).get("version") == "0.74.0",
               "the GitLab report names the version trivy --version gave: %s" % scanner)
        # The runs up to the database's download, and that one, find the cache and the
        # home empty.
        downloaded = kinds.index("image") if "image" in kinds else -1
        root = os.path.normcase(os.path.realpath(tree)) + os.sep
        configs, versions = set(), set()
        for at, call in enumerate(calls):
            version, fresh = call["command"][1:2] == ["--version"], at <= downloaded
            environment = call["env"] if call["env"] is not None else dict(os.environ, **planted)
            given = sorted(name for name in environment if name.upper() in withheld or name.upper().startswith("TRIVY_"))
            expect(not given and environment.get("HM_DEPENDENCY_SCAN_SELF_TEST") == "kept",
                   "trivy %s runs with the job's environment less every variable of Trivy's, whatever its case, and less "
                   "DOCKER_CONFIG, REGISTRY_AUTH_FILE and XDG_RUNTIME_DIR, which name another place for the registry "
                   "configuration, and with the job's others: %s given%s" % (
                       " ".join(call["command"][1:2]), ", ".join(given) or "none",
                       "" if environment.get("HM_DEPENDENCY_SCAN_SELF_TEST") == "kept"
                       else ", and the job's other variables not"))
            for flag, content in (("--config", "{}\n"), ("--ignorefile", "")):
                if version and flag == "--ignorefile":
                    continue
                path, text = call["files"].get(flag, (None, None))
                expect(call["command"].count(flag) == 1 and path and text == content
                       and not os.path.normcase(os.path.realpath(path)).startswith(root),
                       "trivy %s is given %s once, naming an empty file of this script's outside the checkout, so that "
                       "the checkout's own is not read: %s" % (" ".join(call["command"][1:2]), flag,
                                                              "%r holding %r" % (path, text) if path else "not given"))
            path, listed = call["files"].get("--cache-dir", (None, None))
            config = call["files"].get("--config", (None, None))[0]
            expect(call["command"].count("--cache-dir") == 1 and path and listed is not None
                   and (not fresh or listed == [])
                   and config and os.path.dirname(path) == os.path.dirname(config),
                   "trivy %s is given --cache-dir once, naming the directory of this script's beside its configuration, "
                   "empty until the database is downloaded, so that a database of the checkout's is not read: %s" % (
                       " ".join(call["command"][1:2]), "%r holding %s" % (path, listed) if path else "not given"))
            path, listed = call["files"].get("--module-dir", (None, None))
            if version:
                expect("--ignorefile" not in call["command"] and "--module-dir" not in call["command"],
                       "trivy --version is given neither --ignorefile nor --module-dir, which it refuses as unknown "
                       "flags, so that it ends with its version: %s" % call["command"][1:])
            else:
                expect(call["command"].count("--module-dir") == 1 and path and listed == []
                       and config and os.path.dirname(path) == os.path.dirname(config),
                       "trivy %s is given --module-dir once, naming an empty directory of this script's beside its "
                       "configuration, so that no module of the checkout's is loaded: %s" % (
                           " ".join(call["command"][1:2]), "%r holding %s" % (path, listed) if path else "not given"))
            homes = dict((name, value) for name, (value, _) in call["home"].items())
            home, listed = call["home"]["HOME"]
            expect(len(set(homes.values())) == 1 and home and listed is not None
                   and (not fresh or listed == [])
                   and config and os.path.dirname(home) == os.path.dirname(config)
                   and home not in (call["files"].get("--cache-dir", (None,))[0], path),
                   "trivy %s runs with HOME and each XDG base directory naming one more directory of this script's "
                   "beside its configuration, empty until the database is downloaded, so that no default under its home "
                   "is the checkout's: %s" % (" ".join(call["command"][1:2]),
                                               ", ".join("%s=%s" % item for item in sorted(homes.items()))))
            files = tuple(path for path, _ in (call["files"].get(flag, (None, None))
                                               for flag in ("--config", "--ignorefile", "--cache-dir", "--module-dir")))
            if version:
                versions.add((files[0], files[2], home))
            else:
                configs.add(files + (home,))
        expect(len(configs) == 1 and versions == set((paths[0], paths[2], paths[4]) for paths in configs),
               "every run of Trivy is given the same two files, cache, modules and home, and trivy --version the same "
               "configuration, cache and home: %s, and %s" % (sorted(configs, key=str), sorted(versions, key=str)))
        left = [path for paths in configs | versions for path in paths if path and os.path.exists(path)]
        expect(not left and not _own_trivy_files,
               "the files given Trivy are gone once the scan has ended: %s" % (left or _own_trivy_files))
        for name, _ in planted_files:
            os.remove(os.path.join(tree, name))
        for name in (os.path.join(".trivy", "modules"), ".trivy", ".docker"):
            os.rmdir(os.path.join(tree, name))
        module["download_database"], module["trivy"] = (lambda log: TRIVY_DB[0]), fake_trivy

        out = os.path.join(scratch, "read")
        os.makedirs(out)
        quiet = []
        expect(verdict_job(out, {}, quiet.append) == 2, "no verdict: --verdict ends 2")
        expect(quiet[-1:] == [RETRY_NOTE], "no verdict: --verdict says last that a retry of dependency-scan by hand asks "
               "for one of this job: %s" % quiet[-2:])
        for text, what in (("{not json", "a verdict that is not JSON"),
                           (json.dumps({"verdict": "clean", "exit": 0}), "a verdict this does not read"),
                           (json.dumps({"verdict": "passed", "exit": 1}), "a verdict whose exit is not its verdict's"),
                           (json.dumps(["passed"]), "a verdict that is no object")):
            with open(os.path.join(out, VERDICT), "w", encoding="utf-8") as f:
                f.write(text)
            expect(verdict_job(out, {}, quiet.append) == 2, "%s: --verdict ends 2" % what)
        write_verdict(out, "passed", "nothing found", environ={"CI_PIPELINE_ID": "100", "CI_JOB_ID": "7"})
        expect(verdict_job(out, {"CI_PIPELINE_ID": "100"}, quiet.append) == 0, "a verdict of this pipeline's: read")
        expect(verdict_job(out, {"CI_PIPELINE_ID": "101"}, quiet.append) == 2,
               "a verdict another pipeline wrote, read in a pipeline: --verdict ends 2")
        expect(verdict_job(out, {}, quiet.append) == 0, "a pipeline's verdict, read outside one: read")
        write_verdict(out, "passed", "nothing found", environ={})
        expect(verdict_job(out, {"CI_PIPELINE_ID": "101"}, quiet.append) == 2,
               "a verdict written outside a pipeline, read in one - committed to the tree, say: --verdict ends 2")
    finally:
        module["ROOT"], module["installed_trivy"], module["download_database"], module["trivy"] = kept[:4]
        os.chdir(kept[4])
        shutil.rmtree(scratch, ignore_errors=True)
    # The tree: SECURITY.md ("Dependencies") names, as the libraries this scan says
    # UNKNOWN for today, exactly those of VENDORED that OSV holds no usable data on at the
    # tree's pins (without_usable_data), each by the start of its name, and a version it
    # names the one pinned: a pin that moves past its repository's first tag, or a
    # library added, moves the sentence too.
    try:
        policy = " ".join(read("SECURITY.md").split())
    except OSError as e:
        policy = ""
        failures.append("the tree's SECURITY.md is read: %s" % e)
    said = re.search(r"says UNKNOWN, not clean, for a library OSV holds no usable data on \(([^()]*), today\)", policy)
    expect(said, "SECURITY.md says which libraries this scan says UNKNOWN for today (\"... for a library OSV holds no "
           "usable data on (<libraries>, today)\")")
    if said:
        named = []
        for item in re.split(r", | and ", said.group(1)):
            item_name, item_version = re.match(r"^(.*?)(?: ([0-9][0-9A-Za-z.\-]*))?$", item.strip()).groups()
            matched = [lib for lib in VENDORED if lib["name"] == item_name or lib["name"].startswith(item_name + " ")]
            expect(len(matched) == 1, "SECURITY.md's %r, among the libraries the scan says UNKNOWN for, is one library "
                   "of VENDORED: %s" % (item, [lib["name"] for lib in matched]))
            if len(matched) == 1:
                named.append(matched[0]["name"])
                pinned = vendored_version(matched[0])[0]
                expect(item_version is None or item_version == pinned, "SECURITY.md names %s %s among the libraries "
                       "the scan says UNKNOWN for, and the tree pins %s" % (matched[0]["name"], item_version, pinned))
        unknown = [lib["name"] for lib in VENDORED if without_usable_data(lib, vendored_version(lib)[0])]
        expect(sorted(named) == sorted(unknown), "SECURITY.md names %s as what the scan says UNKNOWN for today, and OSV "
               "holds no usable data on %s at the tree's pins (no repository, a control OSV was not known to name, or "
               "a pin older than its repository's first tag)" % (sorted(named), sorted(unknown)))

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", default=os.path.join("security", "dependency-scan"))
    parser.add_argument("--image-root", default="ci-image-packages")
    parser.add_argument("--skip-trivy", action="store_true", help="the OSV step only (no trivy on PATH)")
    parser.add_argument("--skip-osv", action="store_true")
    parser.add_argument("--verdict", action="store_true",
                        help="the dependency-scan-verdict job: the verdict a scan wrote to --out, and the exit it calls for")
    parser.add_argument("--self-test", action="store_true", help="the checks of this script's own reading, offline")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    os.chdir(ROOT)
    if args.verdict:
        return verdict_job(args.out, os.environ)
    try:
        return scan(args)
    finally:
        forget_trivy_files()


def scan(args):
    """The scans, their reports and the verdict, from the root of the checkout: 0 when
    the scans ran, whatever they found, and 2 when one could not."""
    out = args.out
    os.makedirs(out, exist_ok=True)
    log = os.path.join(out, "trivy.log")
    started = now()
    dismissed = vex_not_affected()
    targets, trivy_all, osv_all = [], [], []
    broken = False
    trivy_version = ""

    if not args.skip_trivy:
        trivy_version = installed_trivy()
        if trivy_version is None:
            print("trivy is not on PATH: this runs in the aquasec/trivy image (or pass --skip-trivy)")
            write_verdict(out, "broken", "There was no trivy on PATH, so nothing was scanned.")
            return 2
        print(trivy_version)
        source = download_database(log)
        if not source:
            print("No vulnerability database from %s: nothing can be scanned." % " or ".join(TRIVY_DB))
            write_verdict(out, "broken", "No vulnerability database came from %s, so nothing was scanned." % " or ".join(TRIVY_DB))
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
                found, packages = trivy_findings(image_json, "CI image", IMAGE_PREFIX)
                trivy_all += found
                with open(image_json, encoding="utf-8") as f:
                    os_found = ((json.load(f).get("Metadata") or {}).get("OS") or {})
                os_packages = sum(1 for p in packages if p[1] in ("ubuntu", "debian"))
                live = [f for f in found if not f.get("dismissed")]
                fixable = [f for f in live if blocking(f)]
                text = image_summary(os_found, packages, live, fixable)
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
    # The findings that count first, then the header-only packages' (not affected), then
    # the dismissed, each by severity.
    everything = sorted(trivy_all + osv_all, key=lambda f: (bool(f.get("dismissed")), bool(f.get("not_affected")),
                                                            order.get(f["severity"], 9), f["id"]))
    with open(os.path.join(out, "findings.txt"), "w", encoding="utf-8") as f:
        f.write("".join(describe(x) + "\n" for x in everything))

    print("")
    print("== what was scanned")
    for classname, name, status, text in targets:
        print("%-8s %-10s %s" % (status, classname, name))
        for line in text.split("\n")[:12]:
            print("           " + line)
    live = [f for f in everything if not f.get("dismissed")]
    spared = [f for f in live if f.get("not_affected")]
    held = sum(1 for f in live if blocking(f))
    said = ("%d vulnerabilit%s in all (%d dismissed in the VEX document; %d against the CI image's header-only packages, "
            "%s, %s); %d high or critical with a fixed version." % (
                len(live), "y" if len(live) == 1 else "ies", len(everything) - len(live), len(spared),
                ", ".join(sorted(HEADER_ONLY)), HEADER_ONLY_STATUS, held))
    print("")
    print(said)
    # The verdict, for dependency-scan-verdict: this job ends 0 whenever its scans ran,
    # since GitLab ingests no security report of a job that failed (VERDICT).
    failed = [t for t in targets if t[2] == "FAIL"]
    verdict = "broken" if broken else "failed" if failed else "passed"
    write_verdict(out, verdict, said, failed, held)
    print("")
    print("The verdict, in %s: %s. %s" % (os.path.join(out, VERDICT).replace(os.sep, "/"), verdict, {
        "broken": "A scan could not run, so this job fails, and GitLab takes none of its report.",
        "failed": "%d target(s) failed, which dependency-scan-verdict fails on, allowed to fail; this job passes, so that "
                  "GitLab takes its report into the vulnerability report." % len(failed),
        "passed": "No target failed."}[verdict]))
    return 2 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
