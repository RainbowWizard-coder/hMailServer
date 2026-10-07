# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The advisory database Renovate's vulnerability alerts read, built from OSV's own export.

renovate.json turns on osvVulnerabilityAlerts: for every dependency whose version has a
published advisory, Renovate opens a merge request at once, outside the weekly schedule.
It reads the advisories through @renovatebot/osv-offline, and that package downloads its
database from a GitHub release (renovatebot/osv-offline) unless told otherwise. This
project takes nothing from GitHub. So the renovate job of .gitlab-ci.yml sets
OSV_OFFLINE_DISABLE_DOWNLOAD=true and OSV_OFFLINE_ROOT_DIR, and runs this script first,
which writes the same database from the export OSV itself publishes on Google Cloud
Storage: https://osv-vulnerabilities.storage.googleapis.com/<ecosystem>/all.zip.

The format is the whole of what @renovatebot/osv-offline-db 3.x reads (dist/lib/db.js in
the renovate image): one file per ecosystem, <ecosystem in lower case>.nedb, holding one
advisory per line as JSON. It indexes the lines by affected[].package.name and keeps an
advisory only when that entry's ecosystem and purl are the package's, so an entry with
no purl is given the one osv-offline-db computes (dist/lib/purl-helper.js). Lines are
ASCII, and the only line break is the one after each advisory: the reader seeks to each
advisory by byte offset, counting one byte per line end.

Usage:  python3 build/ci/osv-offline-db.py <directory> <ecosystem>...
        python3 build/ci/osv-offline-db.py "$OSV_OFFLINE_ROOT_DIR" NuGet

The ecosystem names are OSV's (NuGet, npm, PyPI...). Exit 0 when every ecosystem was
written with at least one advisory, 1 otherwise: a run on an empty database would find
no vulnerability and look clean, which is worse than a failed job.
"""

import os
import sys

# Nothing is imported by name from this script's own directory, which Python puts first
# on sys.path: a file put there named as one of Python's modules (json.py, re.py) would
# run as this script's code in a change that leaves the script alone
# (build/check-release-chain.py, PATH_GUARD).
sys.path[:] = [p for p in sys.path if os.path.normcase(os.path.realpath(p or os.curdir)) !=
               os.path.normcase(os.path.dirname(os.path.realpath(__file__)))]

import io
import json
import urllib.parse
import urllib.request
import zipfile

EXPORT = "https://osv-vulnerabilities.storage.googleapis.com/%s/all.zip"

# osv-offline-db's own list (dist/lib/ecosystem.js) and its purl types
# (dist/lib/purl-helper.js).
PURL_TYPES = {
    "crates.io": "cargo",
    "Go": "golang",
    "Hackage": "hackage",
    "Hex": "hex",
    "Maven": "maven",
    "NuGet": "nuget",
    "npm": "npm",
    "Packagist": "composer",
    "PyPI": "pypi",
    "Pub": "pub",
    "RubyGems": "gem",
}


def purl(ecosystem, name):
    """The purl osv-offline-db compares an advisory's entry against."""
    if PURL_TYPES[ecosystem] == "maven":
        name = name.replace(":", "/", 1)
    # encodeURIComponent on each path segment.
    encoded = "/".join(urllib.parse.quote(part, safe="-_.!~*'()") for part in name.split("/"))
    return "pkg:%s/%s" % (PURL_TYPES[ecosystem], encoded)


def fetch(ecosystem):
    request = urllib.request.Request(EXPORT % ecosystem, headers={"User-Agent": "hmailserver-renovate-osv/1"})
    with urllib.request.urlopen(request, timeout=300) as response:
        return response.read()


def write(directory, ecosystem):
    archive = zipfile.ZipFile(io.BytesIO(fetch(ecosystem)))
    target = os.path.join(directory, "%s.nedb" % ecosystem.lower())
    partial = target + ".partial"
    count = 0
    with open(partial, "w", encoding="ascii", newline="\n") as out:
        for member in archive.namelist():
            if not member.endswith(".json"):
                continue
            advisory = json.loads(archive.read(member))
            for affected in advisory.get("affected") or []:
                package = affected.get("package") or {}
                if package.get("ecosystem") == ecosystem and package.get("name") and not package.get("purl"):
                    package["purl"] = purl(ecosystem, package["name"])
            out.write(json.dumps(advisory, ensure_ascii=True, separators=(",", ":")))
            out.write("\n")
            count += 1
    os.replace(partial, target)
    return target, count


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip().splitlines()[0])
        print("Usage: python3 build/ci/osv-offline-db.py <directory> <ecosystem>...")
        return 2
    directory, ecosystems = argv[1], argv[2:]
    unknown = [name for name in ecosystems if name not in PURL_TYPES]
    if unknown:
        print("Not an ecosystem osv-offline reads: %s (it reads %s)." % (", ".join(unknown), ", ".join(sorted(PURL_TYPES))))
        return 2
    os.makedirs(directory, exist_ok=True)
    failed = []
    for ecosystem in ecosystems:
        try:
            target, count = write(directory, ecosystem)
        except Exception as error:  # the message names what failed; the job fails on it
            print("FAIL  %s: %s from %s" % (ecosystem, error, EXPORT % ecosystem))
            failed.append(ecosystem)
            continue
        if count == 0:
            print("FAIL  %s: the export held no advisory." % ecosystem)
            failed.append(ecosystem)
            continue
        print("OK    %s: %d advisories in %s" % (ecosystem, count, target))
    if failed:
        print("No vulnerability alerts can be trusted without %s; stopping before Renovate runs." % ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
