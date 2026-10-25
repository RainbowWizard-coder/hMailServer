#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A software bill of materials for one compiled release asset, and the check that one describes a file.

Every compiled file a release carries - the Windows installer, the .deb, the .rpm,
and the container image when one is published - goes out with a CycloneDX 1.6
document of its own, named after it: hMailServer-6.3.5-x64.exe.cdx.json beside
hMailServer-6.3.5-x64.exe. (OpenSSF Baseline OSPS-QA-02.02.) Its subject
(metadata.component) is that file, by its SHA-256 and SHA-512, so the document
cannot be mistaken for another file's; its components are what that file holds or
needs, and nothing that only another asset carries:

  windows-x64, windows-arm64
      the libraries the server is linked against (OpenSSL, Boost, libpq) and the
      zlib compiled into it, at the versions hMailServer.vcxproj builds with;
      every third-party file the installer packs (the .NET runtime, the MSVC
      runtime, 7-Zip, the MariaDB client, SQL Server Compact), each file by the
      SHA-256 hmailserver/docs/third-party-binaries.json pins it by; and every
      NuGet package of the .NET tools the installer's script packs, at the version
      and with the SHA-512 their packages.lock.json resolves. All of it from
      build/native-dependencies.ps1 (-Platform ARM64 for the ARM64 installer) and
      the lock files, the same inputs the build is held to.
  deb, rpm
      the Linux server's libraries: the distribution's, found at run time (glibc,
      libstdc++, OpenSSL, zlib, libpq, OpenLDAP), Boost linked into the binary, and
      the QuickJS compiled into it. --query-dpkg adds, in the build container,
      the distribution package and version each was built against (and Boost's
      exact version, from its header, since it is in the binary); --read-package
      adds what the package itself declares it depends on (dpkg-deb, rpm).
      A package job of .gitlab/ci/packages-*.yml builds in a distribution's own
      container, which has neither PowerShell for build/native-dependencies.ps1
      nor Python, so it records what its dpkg or rpm had installed and Boost's
      header (build/ci/packages/deb-build.sh, rpm-build.sh), and --dpkg-list or
      --rpm-list reads that record in place of --query-dpkg, in the job that
      makes the document (build/ci/packages/package-sbom.sh).
  appimage
      the Linux server's libraries as for a package, what the build container's
      dpkg recorded each was built against (--dpkg-list, as a package job records
      it: build/ci/packages/appimage-build.sh for x86_64, appimage-cross.sh for the
      cross-compiled aarch64 server), every library the image carries in usr/lib
      (--bundled): by the Ubuntu 24.04 package and version its file came from, and
      the source package and version that was built from, which build-appimage.sh
      copied and gave a RUNPATH, so the copy's bytes are not the package's and the
      package file's SHA-256 is a property, not a hash; and every Alpine package whose
      files the runtime in front of the image links statically (--runtime-linked, the
      runtime job's record of its link), at its version, with its aports commit and
      its licence.
  source
      the source tarball (build/ci/source-tarball.sh): the third-party code and
      data it holds, as hmailserver/docs/SourceTarball.md declares them
      (SOURCE_COMPONENTS), each held to the tarball - its path must be in it - and
      QuickJS at the version its VERSION file in the tarball gives. No dependency
      list and no build container: it is read from the tarball alone.
  image
      Syft's CycloneDX document of one platform's pushed image (every distribution
      package in it, the hmailserver .deb among them), with the .deb's own
      document's components added - what Syft cannot see inside the binary - and
      the image, by its digest and its platform (--arch, linux/amd64 or
      linux/arm64), as the subject. The .deb's document must be the one of that
      architecture's .deb. The container image's jobs make one for each platform
      and attest it to that platform's image with cosign
      (.gitlab/ci/container-image.yml).

The document is the same for the same inputs: its serial number is made from the
file's SHA-256, and its timestamp is SOURCE_DATE_EPOCH's when that is set.

Usage:
  python3 build/asset-sbom.py make --kind windows-x64 --asset hMailServer-6.3.5-x64.exe \\
          --dependencies native-dependencies-x64.json [--out FILE] [--contains NAME=SHA256]...
  python3 build/asset-sbom.py make --kind deb --asset hmailserver_6.3.5_amd64.deb \\
          --dependencies native-dependencies-x64.json --query-dpkg --read-package
  python3 build/asset-sbom.py make --kind deb --asset hmailserver_6.3.6_arm64.deb \\
          --dependencies native-dependencies-x64.json --dpkg-list built-against/dpkg.txt --target-arch arm64 \\
          --boost-header built-against/boost-version.hpp --read-package
  python3 build/asset-sbom.py make --kind rpm --asset hmailserver-6.3.6-1.aarch64.rpm \\
          --dependencies native-dependencies-x64.json --rpm-list built-against/rpm.txt \\
          --boost-header built-against/boost-version.hpp --read-package
  python3 build/asset-sbom.py make --kind appimage --asset hMailServer-6.3.6-x86_64.AppImage \\
          --dependencies native-dependencies-x64.json --dpkg-list built-against-x86_64/dpkg.txt --target-arch amd64 \\
          --boost-header built-against-x86_64/boost-version.hpp --bundled built-against-x86_64/bundled.txt \\
          --runtime-linked built-against-x86_64/runtime-linked.txt
  python3 build/asset-sbom.py make --kind source --asset hmailserver-6.3.6.tar.gz
  python3 build/asset-sbom.py image --syft image.syft.cdx.json --deb-sbom hmailserver_6.3.6_arm64.deb.cdx.json \\
          --image registry.gitlab.com/hmailserver/hmailserver@sha256:<digest> --arch arm64 --version 6.3.6 --out FILE
  python3 build/asset-sbom.py verify FILE.cdx.json FILE      the document describes this very file
  python3 build/asset-sbom.py --self-test                    no network, no dpkg, no rpm

The default --out is the asset's own path with .cdx.json added. How a user finds and
checks these documents: SECURITY.md, "Software bills of materials".
"""

import argparse
import base64
import datetime
import fnmatch
import glob
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = "1.6"
SUFFIX = ".cdx.json"
PROJECT_URL = "https://gitlab.com/hmailserver/hmailserver"
SUPPLIER = {"name": "Progressive Robot Ltd", "url": ["https://www.progressiverobot.com"]}
# What each release file's SBOM says its subject is, after the file's name and kind, and what
# each platform's container image's SBOM says, with the version, the platform and the .deb it
# was built from: the fork, not the original hMailServer (build/check-package-texts.py holds both).
ASSET_DESCRIPTION = "Progressive Robot's fork of hMailServer, a mail server for SMTP, IMAP and POP3"
IMAGE_DESCRIPTION = "The %s container image of Progressive Robot's fork of hMailServer for linux/%s, built from %s"
VERSION_H = os.path.join("hmailserver", "source", "Server", "Common", "Application", "Version.h")
INSTALLATION = os.path.join("hmailserver", "installation")
TOOLS = os.path.join("hmailserver", "source", "Tools")

# Which assets each kind is, which native-dependencies.ps1 target it takes, the
# installer script whose [Files] name the .NET tools it packs, and the components it
# cannot be without: a list missing one of them is a wrong list, not a short one.
KINDS = {
    "windows-x64": {"target": "windows", "files": "section_files_64.iss", "platform": "x64",
                    "required": {"openssl", "boost", "postgresql", "zlib", "dotnet-windowsdesktop-runtime", "msvc-runtime"}},
    "windows-arm64": {"target": "windows", "files": "section_files_arm64.iss", "platform": "ARM64",
                      "required": {"openssl", "boost", "postgresql", "zlib", "dotnet-windowsdesktop-runtime-arm64", "msvc-runtime"}},
    "deb": {"target": "linux", "required": {"linux-glibc", "linux-openssl", "linux-boost", "linux-libpq", "quickjs"}},
    "rpm": {"target": "linux", "required": {"linux-glibc", "linux-openssl", "linux-boost", "linux-libpq", "quickjs"}},
    "appimage": {"target": "linux", "required": {"linux-glibc", "linux-openssl", "linux-boost", "linux-libpq", "quickjs"}},
    "source": {"target": "source", "required": set()},
}

# What the source tarball holds that is not the project's own, as
# hmailserver/docs/SourceTarball.md ("What stays that is not the project's own")
# declares it: (bom-ref, name, the path in the tree, licence as SPDX or None, the
# licence's name when SPDX has no identifier for it, what it is). A path the tarball
# does not hold is refused: the document would describe another tarball. QuickJS's
# version is read from its VERSION file in the tarball.
SOURCE_COMPONENTS = [
    ("quickjs", "QuickJS", "hmailserver/source/Server/quickjs", "MIT", None,
     "Fabrice Bellard's JavaScript engine, compiled into the Linux server"),
    ("mime", "MIME encoder and decoder (Jeff Lee)", "hmailserver/source/Server/Common/Mime", None, "BSD-style notice",
     "the MIME message encoder and analyser"),
    ("cstdstring", "CStdString (Joseph M. O'Leary)", "hmailserver/source/Server/Common/Util/StdString.h", None,
     "its own permissive notice", "the string class"),
    ("rfc7208-tests", "RFC 7208 test suite", "hmailserver/source/Server/SMTP/SPF/Conformance/rfc7208-tests.yml", None,
     "BSD-style notice", "the SPF conformance tests spf-tests runs"),
    ("public-suffix-list", "Public Suffix List", "hmailserver/source/Server/Common/AntiSpam/DMARC/PublicSuffixListData.h",
     "MPL-2.0", None, "the list's rules, compiled into the server"),
    ("unicode-idna-data", "Unicode Character Database for UTS 46 (Unicode 18.0.0)", "hmailserver/source/Server/Common/Idna/ucd",
     "Unicode-3.0", None, "the IDNA mapping table and the normalisation, bidi and joining data, compiled into the server as tables"),
    ("whatwg-html-named-references", "HTML named character references (the WHATWG HTML standard's entities.json)",
     "hmailserver/source/Server/Common/AntiSpam/whatwg", "BSD-3-Clause", None,
     "every named reference HTML defines, compiled into the server as the table its link reader reads an HTML part's references with"),
    ("feather-icons", "Feather icons (Cole Bemis)", "hmailserver/source/Server/Common/Util/Portal.html", "MIT", None,
     "the icons the webmail's page and the Control Deck's page are drawn after, the webmail's compiled into the server"),
    ("atl-script-host", "ATL Active Script host (VisionTech Limited)", "hmailserver/source/Server/Common/Scripting/ScriptSite.h",
     None, "its own permissive notice", "compiled into hMailServer.exe only"),
    ("commonmark-js", "commonmark.js 0.31.2 (John MacFarlane)", "build/check-release-chain.py", "BSD-2-Clause", None,
     "parts of its block and inline parsers, ported to Python in the check of the release jobs"),
    ("gitlab-secret-detection-rules", "GitLab secret detection rules 0.25.1 (GitLab B.V.)", "build/check-gitleaksignore.py",
     "MIT", None, "two of its rules' regular expressions, which the check of .gitleaksignore holds"),
    ("contributor-covenant", "Contributor Covenant 2.1 (Coraline Ada Ehmke and its contributors)", "CODE_OF_CONDUCT.md",
     "CC-BY-4.0", None, "the text the project's code of conduct is adapted from"),
    ("mail-dkim-corpus", "DKIM test messages (Mail::DKIM's corpus)", "hmailserver/test/TestData/DKIM",
     "GPL-1.0-or-later OR Artistic-1.0-Perl", None, "test data the Windows suite and the fuzzing seeds read"),
]
QUICKJS_VERSION_FILE = "hmailserver/source/Server/quickjs/VERSION"

# The Ubuntu development packages the build image builds each Linux component
# against, most likely first; a name may be a dpkg-query pattern. What --query-dpkg
# asks dpkg for.
BUILT_AGAINST = {
    "linux-glibc": ["libc6-dev"],
    "linux-libstdcxx": ["libstdc++-*-dev"],
    "linux-openssl": ["libssl-dev"],
    "linux-zlib": ["zlib1g-dev"],
    "linux-boost": ["libboost-dev", "libboost-thread-dev", "libboost*-dev"],
    "linux-libpq": ["libpq-dev"],
    "linux-openldap": ["libldap-dev", "libldap2-dev"],
}
# A cross build's own C and C++ libraries (deb-build.sh's arm64 route): the cross
# compiler takes their headers, start files and libraries from the release's
# <arch>-cross packages, which crossbuild-essential-<arch> installs, before the
# target architecture's own -dev packages. Every other library is the target
# architecture's -dev package. What --dpkg-list looks for first.
CROSS_TOOLCHAIN = {
    "linux-glibc": "libc6-dev-%s-cross",
    "linux-libstdcxx": "libstdc++-*-dev-%s-cross",
}
# The development packages an RPM distribution builds each Linux component
# against, by what they provide, most likely first: Fedora's zlib-devel is
# provided by zlib-ng-compat-devel, and openSUSE names OpenSSL's, libpq's and
# OpenLDAP's otherwise (rpm-build.sh's sys_pkgs). What --rpm-list looks for.
BUILT_AGAINST_RPM = {
    "linux-glibc": ["glibc-devel"],
    "linux-libstdcxx": ["libstdc++-devel"],
    "linux-openssl": ["openssl-devel", "libopenssl-devel"],
    "linux-zlib": ["zlib-devel"],
    "linux-boost": ["boost-devel"],
    "linux-libpq": ["libpq-devel", "postgresql-devel"],
    "linux-openldap": ["openldap-devel", "openldap2-devel", "openldap2_6-devel"],
}
BOOST_HEADER = "/usr/include/boost/version.hpp"

HASH_LENGTHS = {"SHA-1": 40, "SHA-256": 64, "SHA-384": 96, "SHA-512": 128}


class SbomError(Exception):
    pass


# ---------------------------------------------------------------- small pieces

def digests(path):
    sha256, sha512 = hashlib.sha256(), hashlib.sha512()
    size = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            sha256.update(chunk)
            sha512.update(chunk)
            size += len(chunk)
    return sha256.hexdigest(), sha512.hexdigest(), size


def read_text(path):
    with io.open(path, encoding="utf-8-sig") as f:
        return f.read()


def read_json(path):
    try:
        return json.loads(read_text(path))
    except (OSError, ValueError) as e:
        raise SbomError("%s cannot be read as JSON: %s" % (path, e))


def read_version(root):
    path = os.path.join(root, VERSION_H)
    match = re.search(r'^#define HMAILSERVER_VERSION "([^"]+)"', read_text(path), re.MULTILINE)
    if not match:
        raise SbomError("%s defines no HMAILSERVER_VERSION" % VERSION_H)
    return match.group(1)


def commit_of(root):
    if os.environ.get("CI_COMMIT_SHA"):
        return os.environ["CI_COMMIT_SHA"]
    try:
        answer = subprocess.run(["git", "-C", root, "rev-parse", "HEAD"], capture_output=True, text=True)
        if answer.returncode == 0 and re.match(r"^[0-9a-f]{40}$", answer.stdout.strip()):
            return answer.stdout.strip()
    except OSError:
        pass
    return ""


def timestamp():
    epoch = os.environ.get("SOURCE_DATE_EPOCH", "").strip()
    moment = (datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc) if epoch.isdigit()
              else datetime.datetime.now(datetime.timezone.utc))
    return moment.strftime("%Y-%m-%dT%H:%M:%SZ")


def prop(name, value):
    return {"name": "hmailserver:" + name, "value": str(value)}


def licenses_of(entry):
    """An SPDX expression as an expression, an identifier as an id, and a licence SPDX
    has no identifier for by its name - as build/merge-native-dependencies-into-sbom.ps1 does."""
    text = (entry.get("license") or "").strip()
    if " " in text:
        return [{"expression": text}]
    if text and text != "NOASSERTION":
        return [{"license": {"id": text}}]
    if entry.get("licenseName"):
        return [{"license": {"name": entry["licenseName"]}}]
    return []


def safe_ref(text):
    return re.sub(r"[^A-Za-z0-9._\-+@:/]", "-", text)


# ---------------------------------------------------------------- the components

def native_components(dependencies, kind):
    """The components of native-dependencies.ps1's list that this kind of asset
    carries, as CycloneDX components."""
    target = KINDS[kind]["target"]
    chosen = []
    for entry in dependencies:
        targets = entry.get("targets")
        if targets is None:
            raise SbomError("the dependency list has no targets for %s: it was made by a native-dependencies.ps1 older "
                            "than this script; make it again from the same tree" % entry.get("id"))
        if isinstance(targets, str):
            targets = [targets]
        if target in targets:
            chosen.append(entry)
    have = set(e.get("id") for e in chosen)
    missing = sorted(KINDS[kind]["required"] - have)
    if missing:
        raise SbomError("the dependency list gives a %s asset no %s: that is a wrong list, not a short one, and no SBOM is "
                        "made from it (build/native-dependencies.ps1%s)" % (
                            kind, ", ".join(missing), " -Platform ARM64" if kind == "windows-arm64" else ""))

    components = []
    for entry in chosen:
        ref = "native-" + safe_ref(entry["id"])
        component = {"type": "library", "bom-ref": ref, "name": entry.get("name") or entry["id"]}
        if entry.get("version"):
            component["version"] = str(entry["version"])
        component["description"] = entry.get("why") or ""
        component["scope"] = "required"
        hashes = []
        for h in entry.get("hashes") or []:
            if h and h.get("algorithm") in HASH_LENGTHS and h.get("value"):
                hashes.append({"alg": h["algorithm"], "content": h["value"].lower()})
        if hashes:
            component["hashes"] = hashes
        found = licenses_of(entry)
        if found:
            component["licenses"] = found
        if entry.get("cpe"):
            component["cpe"] = entry["cpe"]
        component["purl"] = entry["purl"]
        if entry.get("website"):
            component["externalReferences"] = [{"type": "website", "url": entry["website"]}]
        properties = [prop("kind", entry.get("kind") or ""), prop("source", entry.get("source") or "")]
        if entry.get("linkage"):
            properties.append(prop("linkage", entry["linkage"]))
        if entry.get("vendored"):
            properties.append(prop("vendored-at", entry["vendored"]))
        component["properties"] = properties
        # Each file the installer packs, by the SHA-256 the manifest pins it by.
        files = []
        for packed in entry.get("files") or []:
            if not packed or not re.match(r"^[0-9a-f]{64}$", str(packed.get("sha256") or "")):
                continue
            files.append({"type": "file", "bom-ref": ref + "/" + safe_ref(packed["path"]), "name": packed["path"],
                          "hashes": [{"alg": "SHA-256", "content": packed["sha256"]}],
                          "properties": [prop("packed-from", "hmailserver/installation/" + packed["path"])]})
        if files:
            component["components"] = sorted(files, key=lambda c: c["name"])
        components.append(component)
    return components


def packed_tools(root, kind):
    """The .NET tools the installer's script packs: each ..\\source\\Tools\\<name>\\publish*\\* source."""
    script = os.path.join(root, INSTALLATION, KINDS[kind]["files"])
    names = re.findall(r'^Source:\s*"\.\.\\source\\Tools\\([A-Za-z0-9_.]+)\\publish(?:-arm64)?\\\*"', read_text(script), re.MULTILINE)
    if not names:
        raise SbomError("%s packs no .NET tool (no Source: \"..\\source\\Tools\\<name>\\publish\\*\"): the installer "
                        "script changed shape, and the SBOM would leave out every NuGet package" % KINDS[kind]["files"])
    return sorted(set(names))


def nuget_components(root, kind):
    """Every NuGet package the packed tools resolve, from their lock files."""
    packages = {}
    for tool in packed_tools(root, kind):
        lock = os.path.join(root, TOOLS, tool, "packages.lock.json")
        if not os.path.isfile(lock):
            raise SbomError("%s packs %s, which has no packages.lock.json to read its packages from" % (KINDS[kind]["files"], tool))
        document = read_json(lock)
        frameworks = [k for k in (document.get("dependencies") or {}) if "/" not in k]
        if not frameworks:
            raise SbomError("%s's packages.lock.json resolves no target framework" % tool)
        for framework in frameworks:
            for name, entry in sorted(document["dependencies"][framework].items()):
                if entry.get("type") == "Project":
                    continue
                version = entry.get("resolved")
                if not version:
                    raise SbomError("%s's packages.lock.json resolves no version of %s" % (tool, name))
                key = (name, version)
                record = packages.setdefault(key, {"hash": entry.get("contentHash") or "", "tools": set(), "direct": False})
                record["tools"].add(tool)
                record["direct"] = record["direct"] or entry.get("type") == "Direct"
    components = []
    for (name, version), record in sorted(packages.items(), key=lambda item: (item[0][0].lower(), item[0][1])):
        component = {"type": "library", "bom-ref": "nuget-%s@%s" % (safe_ref(name), safe_ref(version)), "name": name,
                     "version": version, "scope": "required", "purl": "pkg:nuget/%s@%s" % (name, version)}
        if record["hash"]:
            try:
                raw = base64.b64decode(record["hash"], validate=True)
            except (ValueError, TypeError):
                raise SbomError("the lock files give %s %s a contentHash that is not base64" % (name, version))
            if len(raw) != 64:
                raise SbomError("the lock files give %s %s a contentHash that is not a SHA-512" % (name, version))
            component["hashes"] = [{"alg": "SHA-512", "content": raw.hex()}]
        component["properties"] = [prop("kind", "nuget"), prop("used-by", ", ".join(sorted(record["tools"]))),
                                   prop("reference", "direct" if record["direct"] else "transitive")]
        components.append(component)
    if not components:
        raise SbomError("the packed tools' lock files resolve no NuGet package")
    return components


def default_run(arguments):
    answer = subprocess.run(arguments, capture_output=True, text=True)
    return answer.returncode, answer.stdout, answer.stderr


def boost_from_header(component, boost_header):
    """Boost's exact version, from the version.hpp the build compiled against: it is
    linked into the binary, so its own version is the one the file carries."""
    try:
        header = read_text(boost_header)
    except OSError:
        raise SbomError("%s cannot be read: Boost is linked into the binary, and its version is read from there" % boost_header)
    match = re.search(r"^#define BOOST_VERSION (\d+)\s*$", header, re.MULTILINE)
    if not match:
        raise SbomError("%s defines no BOOST_VERSION" % boost_header)
    number = int(match.group(1))
    component["version"] = "%d.%d.%d" % (number // 100000, number // 100 % 1000, number % 100)
    component["purl"] = "pkg:generic/boost@" + component["version"]


def dpkg_built_against(components, run=default_run, boost_header=BOOST_HEADER):
    """Each Linux component's development package and version, as dpkg has them
    installed where the build ran, and Boost's own version from its header."""
    for component in components:
        ref = component["bom-ref"][len("native-"):]
        for candidate in BUILT_AGAINST.get(ref, []):
            code, out, err = run(["dpkg-query", "-W", "-f", "${db:Status-Abbrev} ${Package} ${Version}\\n", candidate])
            installed = []
            for line in out.splitlines():
                fields = line.split()
                if len(fields) == 3 and fields[0] == "ii":
                    installed.append((fields[1], fields[2]))
            if installed:
                # The highest-numbered, where a pattern matches several (libstdc++-14-dev, -15-dev).
                installed.sort(key=lambda p: [int(n) for n in re.findall(r"\d+", p[0])] or [0])
                package, version = installed[-1]
                component["properties"].append(prop("built-against", "%s %s" % (package, version)))
                break
        else:
            if ref in BUILT_AGAINST:
                raise SbomError("dpkg has none of %s installed, which %s is built against: --query-dpkg runs where the "
                                "package was built" % (", ".join(BUILT_AGAINST[ref]), component["name"]))
        if ref == "linux-boost":
            boost_from_header(component, boost_header)


# ---------------------------------------------------------------- what a package job recorded

def read_dpkg_list(path):
    """What the build container's dpkg had installed, as a package job records it
    (build/ci/packages/deb-build.sh): every line of
    dpkg-query -W -f '${db:Status-Abbrev} ${Package} ${Architecture} ${Version}\\n'.
    [(package, architecture, version)] of what is installed ("ii")."""
    try:
        text = read_text(path)
    except OSError as e:
        raise SbomError("%s cannot be read: %s. The package job records it where it built the package" % (path, e))
    installed = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[0] == "ii":
            installed.append((fields[1], fields[2], fields[3]))
    if not installed:
        raise SbomError("%s records no installed package: it is not what dpkg-query -W printed in the build container" % path)
    return installed


def read_rpm_list(path):
    """What the build's rpm database held, as a package job records it
    (build/ci/packages/rpm-build.sh): every line of
    rpm -qa --qf '[%{=NAME} %{=ARCH} %{=VERSION}-%{=RELEASE} %{PROVIDENAME}\\n]', one per
    thing a package provides, of the aarch64 sysroot in a cross build.
    [(name, arch, version-release, set of what it provides)]."""
    try:
        text = read_text(path)
    except OSError as e:
        raise SbomError("%s cannot be read: %s. The package job records it where it built the package" % (path, e))
    packages = {}
    for line in text.splitlines():
        fields = line.split(None, 3)
        if len(fields) != 4:
            continue
        packages.setdefault((fields[0], fields[1], fields[2]), set()).add(fields[3].strip())
    if not packages:
        raise SbomError("%s records no installed package: it is not what rpm -qa printed where the package was built" % path)
    return [(name, arch, version, provides) for (name, arch, version), provides in sorted(packages.items())]


def dpkg_candidates(ref, arch):
    """The packages --dpkg-list looks for, most likely first: the cross toolchain's
    own, for the C and C++ libraries, then BUILT_AGAINST's packages of the target
    architecture, each named with it (libssl-dev:arm64), so that the build
    container's own architecture's package of the same name is never taken."""
    wanted = ["%s:%s" % (name, arch) for name in BUILT_AGAINST.get(ref, [])]
    if ref in CROSS_TOOLCHAIN:
        wanted.insert(0, CROSS_TOOLCHAIN[ref] % arch)
    return wanted


def recorded_built_against(components, installed, kind, arch=None, boost_header=BOOST_HEADER, where="the record"):
    """dpkg_built_against, from what a package job recorded rather than from the dpkg
    here: each Linux component's development package and version, and Boost's
    version from the header the job copied. installed is read_dpkg_list's (kind
    "dpkg", with the target architecture arch) or read_rpm_list's (kind "rpm")."""
    for component in components:
        ref = component["bom-ref"][len("native-"):]
        if kind == "dpkg":
            wanted = dpkg_candidates(ref, arch)
        else:
            wanted = BUILT_AGAINST_RPM.get(ref, [])
        for candidate in wanted:
            if kind == "dpkg":
                name, _, qualifier = candidate.partition(":")
                found = [(package, "%s:%s" % (package, architecture) if qualifier else package, version)
                         for package, architecture, version in installed
                         if fnmatch.fnmatchcase(package, name) and (not qualifier or architecture == qualifier)]
            else:
                found = [(name, name, version) for name, _, version, provides in installed if candidate in provides]
            if found:
                # The highest-numbered, where a pattern matches several, as dpkg_built_against chooses.
                found.sort(key=lambda p: [int(n) for n in re.findall(r"\d+", p[0])] or [0])
                _, label, version = found[-1]
                component["properties"].append(prop("built-against", "%s %s" % (label, version)))
                break
        else:
            if ref == "linux-boost" and not any(boost_package(entry, kind) for entry in installed):
                # No Boost package at all where the build ran: the job built Boost from
                # boost.org's release tarball, by its pinned SHA-256, where the
                # distribution's is older than the build needs or ships no static
                # archives (deb-build.sh for Debian 12, rpm-build.sh for EL and
                # openSUSE). The header the job kept is that build's, so its version
                # is the one in the binary, and that is what the document says.
                component["properties"].append(prop("built-against", "Boost built from boost.org's release tarball "
                                                    "in the package job (no Boost development package where the build ran)"))
            elif wanted:
                raise SbomError("%s records none of %s installed, which %s is built against: the package job records "
                                "what its build container had" % (where, ", ".join(wanted), component["name"]))
        if ref == "linux-boost":
            boost_from_header(component, boost_header)


def boost_package(entry, kind):
    """Whether one package of a record is a Boost development package, one with
    headers a build could have compiled against: then a record that names none of
    BUILT_AGAINST's is a wrong record, not a Boost built from source. A package of
    Boost's shared libraries or its licence holds no header, and no build is against
    it: openSUSE's build containers carry libboost_thread1_86_0 and
    boost-license1_86_0 (1_92_0 on Tumbleweed), which the tools the job installs
    pull in, while the job builds Boost from boost.org's tarball (rpm-build.sh)."""
    name = entry[0]
    if kind == "rpm":
        return (name.startswith(("boost", "libboost")) and name.endswith("-devel")) \
            or any(p.startswith("boost-devel") for p in entry[3])
    return name.startswith("libboost") and name.endswith("-dev")


def read_bundled(path, arch):
    """What an AppImage carries in usr/lib, as build/ci/packages/appimage-build.sh
    records it (bundled.txt): one line per library, "<soname> <package>:<arch>
    <version> <sha256 of the package's file> <source package> <source version>", from
    the dpkg of the container that packed it. [(soname, package, version, sha256,
    source, source version)]. A library whose package dpkg could not name, or whose
    source it could not, is refused: the document would leave out what the image
    holds, or where its source is."""
    try:
        text = read_text(path)
    except OSError as e:
        raise SbomError("%s cannot be read: %s. The AppImage job records it as it packs the image" % (path, e))
    bundled = []
    for number, line in enumerate(text.splitlines(), 1):
        fields = line.split()
        if not fields:
            continue
        if len(fields) != 6 or not re.match(r"^[A-Za-z0-9._+\-]+\.so(\.[0-9]+)*$", fields[0]) \
                or not re.match(r"^[a-z0-9][a-z0-9.+\-]*:[a-z0-9]+$", fields[1]) or not re.match(r"^[0-9a-f]{64}$", fields[3]) \
                or not re.match(r"^[a-z0-9][a-z0-9.+\-]+$", fields[4]):
            raise SbomError("%s line %d is not '<soname> <package>:<arch> <version> <sha256> <source> <source version>': %r"
                            % (path, number, line))
        package, _, package_arch = fields[1].partition(":")
        if package_arch not in (arch, "all"):
            raise SbomError("%s line %d: %s is a package of %s, not of %s, the image's" % (path, number, fields[1], package_arch, arch))
        bundled.append((fields[0], package, fields[2], fields[3], fields[4], fields[5]))
    names = [b[0] for b in bundled]
    if len(names) != len(set(names)):
        raise SbomError("%s names a library twice" % path)
    return bundled


def bundled_components(bundled, arch):
    """A component for each library an AppImage carries: the Ubuntu 24.04 package
    its file came from, and the source package and version that was built from,
    whose source the release file hmailserver-<v>-appimage-bundled-sources.tar.gz
    carries. The copy in usr/lib has a RUNPATH of build-appimage.sh's and so other
    bytes than the package's file, whose SHA-256 is given as a property; the image's
    own SHA-256 is the subject's."""
    components = []
    for soname, package, version, sha256, source, source_version in bundled:
        components.append({
            "type": "library", "bom-ref": "bundled-" + safe_ref(soname), "name": package, "version": version,
            "description": "%s, bundled in the AppImage's usr/lib" % soname, "scope": "required",
            "purl": "pkg:deb/ubuntu/%s@%s?arch=%s&distro=ubuntu-24.04" % (package, version, arch),
            "properties": [prop("kind", "bundled"), prop("bundled-as", "usr/lib/" + soname),
                           prop("package-file-sha256", sha256), prop("linkage", "dynamic, from the image"),
                           prop("source-package", "%s %s" % (source, source_version)),
                           prop("copyright-file", "usr/share/hmailserver/licenses/bundled/%s/copyright" % package)],
        })
    return components


# The AppImage runtime's architectures as the record names them (uname -m), and the
# image's as dpkg does.
RUNTIME_ARCHES = {"amd64": "x86_64", "arm64": "aarch64"}


def read_runtime_linked(path, arch):
    """What the runtime at the front of an AppImage links statically, as its job
    recorded it from the linker's map (build/ci/packages/appimage-runtime-sources.sh
    record, runtime-<arch>-linked.txt, which appimage-build.sh keeps as
    built-against-<arch>/runtime-linked.txt): "alpine <release>", "arch <machine>" and
    one line per file, "linked <path> <package> <version> <origin> <aports commit>
    <licence...>". (release, [(path, package, version, origin, commit, licence)]). A
    record of another architecture, of no file, or without libfuse3, is refused."""
    try:
        text = read_text(path)
    except OSError as e:
        raise SbomError("%s cannot be read: %s. The runtime's job records it from the link" % (path, e))
    release, machine, linked = None, None, []
    for number, line in enumerate(text.splitlines(), 1):
        fields = line.split()
        if not fields or line.startswith("#"):
            continue
        if fields[0] == "alpine" and len(fields) == 2 and re.match(r"^[0-9]+(\.[0-9]+)+$", fields[1]):
            release = fields[1]
        elif fields[0] == "arch" and len(fields) == 2:
            machine = fields[1]
        elif fields[0] == "linked" and len(fields) >= 7 and fields[1].startswith("/") \
                and re.match(r"^[a-z0-9][a-z0-9._+\-]*$", fields[2]) and re.match(r"^[0-9][A-Za-z0-9._+~\-]*-r[0-9]+$", fields[3]) \
                and re.match(r"^[a-z0-9][a-z0-9._+\-]*$", fields[4]) and re.match(r"^[0-9a-f]{40}$", fields[5]):
            linked.append((fields[1], fields[2], fields[3], fields[4], fields[5], " ".join(fields[6:])))
        else:
            raise SbomError("%s line %d is not 'alpine <release>', 'arch <machine>' or 'linked <path> <package> <version> "
                            "<origin> <commit> <licence>': %r" % (path, number, line))
    if not release or machine != RUNTIME_ARCHES.get(arch):
        raise SbomError("%s is the record of a runtime for %s on Alpine %s, not of the %s AppImage's" % (
            path, machine, release, RUNTIME_ARCHES.get(arch, arch)))
    if not linked or not any(entry[3] == "fuse3" for entry in linked):
        raise SbomError("%s records no link of libfuse3: it is not the record of this runtime's link" % path)
    return release, linked


def runtime_components(release, linked, arch):
    """A component for each Alpine package whose files the AppImage's runtime links
    statically, at its version, with its aports origin and commit, its licence as
    apk's database gives it (Alpine's, an SPDX-like expression, given as a name) and
    each file linked. Their source is the release file
    hmailserver-<v>-appimage-runtime-sources.tar.gz but GCC's, whose start-up files
    are under the GCC Runtime Library Exception."""
    grouped = {}
    for path, package, version, origin, commit, licence in linked:
        entry = grouped.setdefault((package, version), {"origin": origin, "commit": commit, "licence": licence, "files": []})
        if (entry["origin"], entry["commit"], entry["licence"]) != (origin, commit, licence):
            raise SbomError("the runtime's record gives %s %s two origins, commits or licences" % (package, version))
        entry["files"].append(path)
    components = []
    for (package, version), entry in sorted(grouped.items()):
        components.append({
            "type": "library", "bom-ref": "runtime-" + safe_ref(package), "name": package, "version": version,
            "description": "linked statically into the AppImage's runtime: %s" % ", ".join(sorted(entry["files"])),
            "scope": "required", "licenses": [{"license": {"name": entry["licence"]}}],
            "purl": "pkg:apk/alpine/%s@%s?arch=%s&distro=alpine-%s" % (package, version, RUNTIME_ARCHES[arch], release),
            "properties": [prop("kind", "runtime-static"), prop("linkage", "static, into the AppImage's runtime"),
                           prop("aports-origin", entry["origin"]), prop("aports-commit", entry["commit"])]
                          + [prop("linked-file", f) for f in sorted(entry["files"])],
        })
    return components


def source_components(asset, version):
    """The source tarball's third-party parts (SOURCE_COMPONENTS), each held to the
    tarball, and the tarball's own version: its one top directory is
    hmailserver-<version>/ (build/check-source-tarball.py holds the rest of it)."""
    try:
        archive = tarfile.open(asset, "r:gz")
        members = archive.getmembers()
    except (OSError, tarfile.TarError, EOFError) as e:
        raise SbomError("%s cannot be read as a gzip tarball: %s" % (asset, e))
    top = "hmailserver-%s/" % version
    names = set()
    for member in members:
        if not (member.name + "/").startswith(top):
            archive.close()
            raise SbomError("%s holds %s, outside %s: it is not the source tarball of %s" % (asset, member.name, top, version))
        names.add(member.name[len(top):])
    quickjs = None
    try:
        member = archive.getmember(top + QUICKJS_VERSION_FILE)
        quickjs = archive.extractfile(member).read().decode("utf-8", "replace").strip()
    except (KeyError, AttributeError):
        pass
    finally:
        archive.close()
    if not quickjs or not re.match(r"^[0-9][0-9.\-]*$", quickjs):
        raise SbomError("%s has no %s to read QuickJS's version from" % (asset, QUICKJS_VERSION_FILE))
    components = []
    for ref, name, path, spdx, license_name, what in SOURCE_COMPONENTS:
        if path not in names and not any(n.startswith(path + "/") for n in names):
            raise SbomError("%s does not hold %s, which docs/SourceTarball.md declares (%s): the document would describe "
                            "another tarball" % (asset, path, name))
        component = {"type": "library", "bom-ref": "source-" + ref, "name": name, "description": what, "scope": "required",
                     "properties": [prop("kind", "vendored"), prop("vendored-at", path)]}
        if ref == "quickjs":
            component["version"] = quickjs
            component["purl"] = "pkg:generic/quickjs@%s" % quickjs
        else:
            component["purl"] = "pkg:generic/hmailserver-vendored/%s@%s" % (ref, version)
        if spdx:
            component["licenses"] = [{"expression": spdx}] if " " in spdx else [{"license": {"id": spdx}}]
        else:
            component["licenses"] = [{"license": {"name": license_name}}]
        components.append(component)
    return components


def package_facts(kind, asset, run=default_run):
    """The package's own version (a distribution's build carries a suffix of its own,
    6.3.5-1~debian13) and what it says it needs installed: the .deb's Depends, the
    .rpm's Requires, each once, in the package's order."""
    if kind == "deb":
        code, out, err = run(["dpkg-deb", "-f", asset, "Version", "Depends"])
        if code != 0:
            raise SbomError("dpkg-deb could not read %s: %s" % (asset, (err or out).strip()[:300]))
        fields, name = {}, None
        for line in out.splitlines():
            match = re.match(r"^([A-Za-z-]+):\s*(.*)$", line)
            if match:
                name = match.group(1)
                fields[name] = match.group(2)
            elif name and line.startswith(" "):
                fields[name] += " " + line.strip()
        version = fields.get("Version", "").strip()
        items = [part.strip() for part in fields.get("Depends", "").split(",") if part.strip()]
    else:
        code, out, err = run(["rpm", "-qp", "--queryformat", "%{VERSION}-%{RELEASE}\\n", asset])
        if code != 0:
            raise SbomError("rpm could not read %s: %s" % (asset, (err or out).strip()[:300]))
        version = out.strip().splitlines()[0].strip() if out.strip() else ""
        code, out, err = run(["rpm", "-qp", "--requires", asset])
        if code != 0:
            raise SbomError("rpm could not read %s: %s" % (asset, (err or out).strip()[:300]))
        items = [line.strip() for line in out.splitlines() if line.strip()]
    if not version:
        raise SbomError("%s names no version of its own" % asset)
    unique = []
    for item in items:
        if item not in unique:
            unique.append(item)
    return version, unique


# ---------------------------------------------------------------- the document

def subject(asset_name, kind, version, sha256, sha512, size, commit, declared=None, contains=None, package_version=None):
    names = {"windows-x64": "the Windows installer for x64", "windows-arm64": "the Windows installer for ARM64",
             "deb": "the Debian and Ubuntu package", "rpm": "the RPM package", "image": "the container image",
             "appimage": "the AppImage", "source": "the source tarball"}
    if kind == "deb":
        arch = re.search(r"_([a-z0-9]+)\.deb$", asset_name)
        purl = "pkg:deb/hmailserver@%s%s" % (package_version or version, "?arch=" + arch.group(1) if arch else "")
    elif kind == "rpm":
        arch = re.search(r"\.([a-z0-9_]+)\.rpm$", asset_name)
        release = re.search(r"-%s-([0-9]+)\." % re.escape(version), asset_name)
        purl = "pkg:rpm/hmailserver@%s%s" % (package_version or (version + ("-" + release.group(1) if release else "")),
                                             "?arch=" + arch.group(1) if arch else "")
    else:
        purl = "pkg:generic/hmailserver@%s?file_name=%s" % (version, asset_name)
    component = {
        "type": "application", "bom-ref": "asset", "supplier": SUPPLIER, "name": "hMailServer", "version": version,
        "description": "%s, %s: %s" % (asset_name, names.get(kind, kind), ASSET_DESCRIPTION),
        "hashes": [{"alg": "SHA-256", "content": sha256}, {"alg": "SHA-512", "content": sha512}],
        "licenses": [{"license": {"id": "AGPL-3.0-or-later"}}],
        "purl": purl,
        "externalReferences": [{"type": "vcs", "url": PROJECT_URL}, {"type": "website", "url": PROJECT_URL}],
        "properties": [prop("asset-file", asset_name), prop("asset-kind", kind), prop("asset-size", size)],
    }
    if commit:
        component["properties"].append(prop("commit", commit))
    if package_version:
        component["properties"].append(prop("package-version", package_version))
    for item in declared or []:
        component["properties"].append(prop("declared-dependency", item))
    if contains:
        component["components"] = [{"type": "file", "bom-ref": "asset/" + safe_ref(name), "name": name,
                                     "hashes": [{"alg": "SHA-256", "content": digest}],
                                     "properties": [prop("origin", "this project's own build")]}
                                    for name, digest in sorted(contains)]
    return component


def document(subject_component, components, sha256, kind, extra_tools=None):
    serial = uuid.uuid5(uuid.NAMESPACE_URL, "%s/sbom/%s/%s" % (PROJECT_URL, kind, sha256))
    tools = [{"type": "application", "group": "hMailServer", "name": "build/asset-sbom.py", "supplier": SUPPLIER,
              "externalReferences": [{"type": "vcs", "url": PROJECT_URL}]}]
    tools.extend(extra_tools or [])
    top = [c["bom-ref"] for c in components]
    bom = {
        "bomFormat": "CycloneDX",
        "specVersion": SPEC,
        "serialNumber": "urn:uuid:%s" % serial,
        "version": 1,
        "metadata": {
            "timestamp": timestamp(),
            "tools": {"components": tools},
            "component": subject_component,
            "supplier": SUPPLIER,
            "properties": [prop("describes", "the compiled release asset named in metadata.component, by its SHA-256; "
                                "how to check it: SECURITY.md, 'Software bills of materials'")],
        },
        "components": components,
        "dependencies": [{"ref": subject_component["bom-ref"], "dependsOn": top}] + [{"ref": ref, "dependsOn": []} for ref in top],
    }
    problems = structure_problems(bom)
    if problems:
        raise SbomError("the document made is not well formed: %s" % "; ".join(problems))
    return bom


def make(kind, asset, dependencies, root=ROOT, version=None, commit=None, contains=None, query_dpkg=False,
         read_package=False, run=default_run, boost_header=BOOST_HEADER, dpkg_list=None, rpm_list=None, target_arch=None,
         bundled=None, runtime_linked=None):
    if kind not in KINDS:
        raise SbomError("%s is not a kind of asset this makes (%s)" % (kind, ", ".join(sorted(KINDS))))
    if not os.path.isfile(asset) or os.path.getsize(asset) == 0:
        raise SbomError("%s is not a file, or is empty" % asset)
    if (query_dpkg or read_package or dpkg_list or rpm_list) and KINDS[kind]["target"] != "linux":
        raise SbomError("--query-dpkg, --dpkg-list, --rpm-list and --read-package are for a .deb or an .rpm, and --dpkg-list for an AppImage too")
    if kind == "appimage" and (read_package or query_dpkg or rpm_list or not dpkg_list or not bundled or not runtime_linked):
        raise SbomError("an AppImage's document is made from its build's record alone: --dpkg-list, --target-arch, "
                        "--bundled and --runtime-linked, which build/ci/packages/appimage-build.sh writes (it has no "
                        "package metadata to read)")
    if bundled and kind != "appimage":
        raise SbomError("--bundled is for an AppImage")
    if runtime_linked and kind != "appimage":
        raise SbomError("--runtime-linked is for an AppImage")
    if kind == "source":
        if contains:
            raise SbomError("a source tarball's document takes nothing but the tarball")
        version = version or read_version(root)
        sha256, sha512, size = digests(asset)
        components = source_components(asset, version)
        component = subject(os.path.basename(asset), kind, version, sha256, sha512, size,
                            commit if commit is not None else commit_of(root))
        return document(component, components, sha256, kind)
    if sum(1 for given in (query_dpkg, dpkg_list, rpm_list) if given) > 1:
        raise SbomError("--query-dpkg, --dpkg-list and --rpm-list are three ways to one answer, what the package was built "
                        "against: give one")
    if dpkg_list and not re.match(r"^[a-z0-9][a-z0-9\-]*$", target_arch or ""):
        raise SbomError("--dpkg-list needs --target-arch, the package's own architecture as dpkg names it (arm64, amd64): "
                        "the build container has its own architecture's packages beside the target's")
    if target_arch and not dpkg_list:
        raise SbomError("--target-arch is for --dpkg-list")
    version = version or read_version(root)
    sha256, sha512, size = digests(asset)
    components = native_components(dependencies, kind)
    if KINDS[kind]["target"] == "windows":
        components += nuget_components(root, kind)
    linux = [c for c in components if c["bom-ref"].startswith("native-linux-")]
    if query_dpkg:
        dpkg_built_against(linux, run, boost_header)
    elif dpkg_list:
        recorded_built_against(linux, read_dpkg_list(dpkg_list), "dpkg", target_arch, boost_header, dpkg_list)
    elif rpm_list:
        recorded_built_against(linux, read_rpm_list(rpm_list), "rpm", None, boost_header, rpm_list)
    if bundled:
        components += bundled_components(read_bundled(bundled, target_arch), target_arch)
    if runtime_linked:
        release, linked = read_runtime_linked(runtime_linked, target_arch)
        components += runtime_components(release, linked, target_arch)
    package_version, declared = package_facts(kind, asset, run) if read_package else (None, None)
    component = subject(os.path.basename(asset), kind, version, sha256, sha512, size,
                        commit if commit is not None else commit_of(root), declared, contains, package_version)
    return document(component, components, sha256, kind)


# The platforms the container image is built for, as dpkg and the OCI name them.
IMAGE_ARCHES = ("amd64", "arm64")


def image_document(syft, deb_sbom, image, version, arch):
    """One platform's pushed image's document: Syft's components, the .deb's own
    added, and the image by its digest and its platform as the subject."""
    match = re.match(r"^([a-z0-9.\-]+(?::[0-9]+)?/[a-z0-9._/\-]+)@sha256:([0-9a-f]{64})$", image or "")
    if not match:
        raise SbomError("the image is %r, not <repository>@sha256:<digest>: an image is described by its digest" % image)
    if arch not in IMAGE_ARCHES:
        raise SbomError("the image's architecture is %r: the image is built for %s" % (arch, " and ".join(IMAGE_ARCHES)))
    if syft.get("bomFormat") != "CycloneDX":
        raise SbomError("Syft's document is not CycloneDX")
    if deb_sbom.get("bomFormat") != "CycloneDX" or (deb_sbom.get("metadata") or {}).get("component", {}).get("properties") is None:
        raise SbomError("the .deb's document is not one this script made")
    kind_of = dict((p["name"], p["value"]) for p in deb_sbom["metadata"]["component"]["properties"])
    if kind_of.get("hmailserver:asset-kind") != "deb":
        raise SbomError("the .deb's document describes a %s, not a .deb" % kind_of.get("hmailserver:asset-kind"))
    # Each platform's image is built from the .deb of its own architecture.
    if not (kind_of.get("hmailserver:asset-file") or "").endswith("_%s.deb" % arch):
        raise SbomError("the .deb's document describes %s, not the %s .deb the linux/%s image is built from"
                        % (kind_of.get("hmailserver:asset-file"), arch, arch))
    components = []
    seen = set()
    for component in syft.get("components") or []:
        ref = component.get("bom-ref") or "syft-%d" % len(components)
        component = dict(component)
        component["bom-ref"] = "syft:" + ref
        seen.add(component["bom-ref"])
        components.append(component)
    for component in deb_sbom.get("components") or []:
        component = json.loads(json.dumps(component))
        component["bom-ref"] = "deb:" + component["bom-ref"]
        for child in component.get("components") or []:
            child["bom-ref"] = "deb:" + child["bom-ref"]
        component.setdefault("properties", []).append(prop("from", "the .deb's own SBOM, %s" % kind_of.get("hmailserver:asset-file")))
        components.append(component)
    repository, digest = match.group(1), match.group(2)
    subject_component = {
        "type": "container", "bom-ref": "asset", "supplier": SUPPLIER, "name": repository, "version": version,
        "description": IMAGE_DESCRIPTION % (version, arch, kind_of.get("hmailserver:asset-file")),
        "hashes": [{"alg": "SHA-256", "content": digest}],
        "licenses": [{"license": {"id": "AGPL-3.0-or-later"}}],
        "purl": "pkg:oci/hmailserver@sha256%%3A%s?arch=%s&repository_url=%s" % (digest, arch, repository),
        "externalReferences": [{"type": "vcs", "url": PROJECT_URL}],
        "properties": [prop("asset-file", image), prop("asset-kind", "image"), prop("platform", "linux/" + arch),
                       prop("built-from", kind_of.get("hmailserver:asset-file"))],
    }
    syft_tools = ((syft.get("metadata") or {}).get("tools") or {})
    extra = syft_tools.get("components") if isinstance(syft_tools, dict) else None
    return document(subject_component, components, digest, "image", extra_tools=extra)


# ---------------------------------------------------------------- checking one

def structure_problems(bom):
    """What makes a CycloneDX document unusable, whatever wrote it: the header, the
    subject's hashes, every bom-ref once, every hash well formed, every dependency
    naming a component that is there."""
    problems = []
    if bom.get("bomFormat") != "CycloneDX" or not str(bom.get("specVersion", "")).startswith("1."):
        problems.append("not a CycloneDX document")
    if not re.match(r"^urn:uuid:[0-9a-f\-]{36}$", str(bom.get("serialNumber", ""))):
        problems.append("no serialNumber urn:uuid")
    subject_component = (bom.get("metadata") or {}).get("component")
    if not isinstance(subject_component, dict):
        return problems + ["no metadata.component: the document names no subject"]
    refs = []

    def walk(component, where):
        refs.append(component.get("bom-ref"))
        if not component.get("name"):
            problems.append("%s has no name" % where)
        for h in component.get("hashes") or []:
            length = HASH_LENGTHS.get(h.get("alg"))
            if not length or not re.match(r"^[0-9a-f]{%d}$" % length, str(h.get("content", ""))):
                problems.append("%s has a hash that is not a well-formed %s" % (where, h.get("alg")))
        for child in component.get("components") or []:
            walk(child, "%s/%s" % (where, child.get("name")))

    walk(subject_component, "the subject")
    for component in bom.get("components") or []:
        walk(component, component.get("name") or "a component")
    if not any(h.get("alg") == "SHA-256" for h in subject_component.get("hashes") or []):
        problems.append("the subject has no SHA-256: nothing ties the document to a file")
    named = [r for r in refs if r]
    if len(named) != len(set(named)):
        problems.append("a bom-ref is used twice")
    for dependency in bom.get("dependencies") or []:
        for ref in [dependency.get("ref")] + list(dependency.get("dependsOn") or []):
            if ref not in named:
                problems.append("a dependency names %s, which is no component's bom-ref" % ref)
    return problems


def verify(sbom_path, asset_path, out=print):
    """0 when the document is well formed and its subject is this file, by SHA-256."""
    bom = read_json(sbom_path)
    problems = structure_problems(bom)
    sha256, _, _ = digests(asset_path)
    subject_component = (bom.get("metadata") or {}).get("component") or {}
    theirs = [h.get("content") for h in subject_component.get("hashes") or [] if h.get("alg") == "SHA-256"]
    if theirs and sha256 not in theirs:
        problems.append("it describes the file whose SHA-256 is %s, and %s is %s" % (theirs[0], os.path.basename(asset_path), sha256))
    for problem in problems:
        out("PROBLEM: %s: %s" % (os.path.basename(sbom_path), problem))
    if problems:
        return 1
    properties = dict((p.get("name"), p.get("value")) for p in subject_component.get("properties") or [])
    out("%s describes %s (SHA-256 %s)%s:" % (os.path.basename(sbom_path), os.path.basename(asset_path), sha256,
                                             ", made as %s" % properties["hmailserver:asset-file"] if properties.get("hmailserver:asset-file") else ""))
    for component in bom.get("components") or []:
        files = len(component.get("components") or [])
        out("  %-40s %-28s %s" % (component.get("name"), component.get("version") or "(the distribution's)",
                                  component.get("purl") or "") + ("  [%d files]" % files if files else ""))
    out("%d components." % len(bom.get("components") or []))
    return 0


# ---------------------------------------------------------------- the self-test

def self_test():
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    def refused(what, needle, call):
        try:
            call()
            failures.append("%s is refused" % what)
        except SbomError as e:
            expect(needle in str(e), "%s is refused, saying why: %s" % (what, e))

    work = tempfile.mkdtemp(prefix="hm-asset-sbom-")
    try:
        def write(relative, data):
            path = os.path.join(work, relative)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data if isinstance(data, bytes) else data.encode("utf-8"))
            return path

        write(VERSION_H, '#define HMAILSERVER_VERSION "6.3.5"\n')
        write(os.path.join(INSTALLATION, "section_files_64.iss"),
              '[Files]\nSource: "..\\source\\Tools\\ControlPanel\\publish\\*"; DestDir: "{app}\\ControlPanel"\n'
              'Source: "..\\source\\Tools\\ImportTool\\publish\\*"; DestDir: "{app}\\Addons\\ImportTool"\n'
              '; Source: "..\\source\\Tools\\Commented\\publish\\*"\n')
        write(os.path.join(INSTALLATION, "section_files_arm64.iss"),
              'Source: "..\\source\\Tools\\ControlPanel\\publish-arm64\\*"; DestDir: "{app}\\ControlPanel"\n')
        sha512 = hashlib.sha512(b"a package").digest()
        lock = {"version": 1, "dependencies": {
            "net10.0-windows7.0": {
                "WPF-UI": {"type": "Direct", "requested": "[4.3.0, )", "resolved": "4.3.0", "contentHash": base64.b64encode(sha512).decode()},
                "SkiaSharp": {"type": "Transitive", "resolved": "3.119.0", "contentHash": base64.b64encode(hashlib.sha512(b"s").digest()).decode()},
                "ControlPanel.Core": {"type": "Project", "dependencies": {}},
            },
            "net10.0-windows7.0/win-x64": {"SkiaSharp": {"type": "Transitive", "resolved": "3.119.0", "contentHash": ""}},
        }}
        write(os.path.join(TOOLS, "ControlPanel", "packages.lock.json"), json.dumps(lock))
        write(os.path.join(TOOLS, "ImportTool", "packages.lock.json"), json.dumps({"version": 1, "dependencies": {"net10.0": {
            "WPF-UI": {"type": "Transitive", "resolved": "4.3.0", "contentHash": base64.b64encode(sha512).decode()},
            "MimeKit": {"type": "Direct", "resolved": "4.12.0", "contentHash": base64.b64encode(hashlib.sha512(b"m").digest()).decode()}}}}))

        def dep(id_, kind, targets, version="1.0", **more):
            entry = {"id": id_, "name": id_, "version": version, "kind": kind, "purl": "pkg:generic/%s@%s" % (id_, version),
                     "cpe": "", "license": "MIT", "licenseName": "", "website": "https://example.org/", "why": "why " + id_,
                     "source": "test", "vendored": None, "hashes": [], "targets": targets, "linkage": "", "files": []}
            entry.update(more)
            return entry

        crt = "a" * 64
        dependencies = [
            dep("openssl", "linked", ["windows"], "4.0.2"), dep("boost", "linked", ["windows"], "1.92.0"),
            dep("postgresql", "linked", ["windows"], "18.3"), dep("zlib", "vendored", ["windows"], "1.3.2"),
            dep("quickjs", "vendored", ["linux"], "2025-04-26"),
            dep("dotnet-windowsdesktop-runtime", "shipped", ["windows"], "10.0.12",
                hashes=[{"algorithm": "SHA-512", "value": "b" * 128}], license="NOASSERTION", licenseName="Microsoft terms"),
            dep("msvc-runtime", "shipped", ["windows"], "14.51", files=[{"path": "Microsoft.VC145.CRT/msvcp140.dll", "sha256": crt}]),
            dep("linux-glibc", "system", ["linux"], "", linkage="dynamic", license="LGPL-2.1-or-later"),
            dep("linux-openssl", "system", ["linux"], "", linkage="dynamic"),
            dep("linux-boost", "system", ["linux"], "", linkage="static", license="BSL-1.0"),
            dep("linux-libpq", "system", ["linux"], "", linkage="dynamic"),
            dep("linux-libstdcxx", "system", ["linux"], "", linkage="dynamic", license="GPL-3.0-or-later WITH GCC-exception-3.1"),
        ]
        installer = write("out/hMailServer-6.3.5-x64.exe", b"MZ an installer")
        other = write("out/other.exe", b"MZ another installer")

        # Windows.
        bom = make("windows-x64", installer, dependencies, root=work, commit="c" * 40, contains=[("hMailServer.exe", "d" * 64)])
        names = [c["name"] for c in bom["components"]]
        expect(bom["specVersion"] == SPEC and bom["bomFormat"] == "CycloneDX", "a CycloneDX %s document" % SPEC)
        expect(bom["metadata"]["component"]["hashes"][0] == {"alg": "SHA-256", "content": hashlib.sha256(b"MZ an installer").hexdigest()},
               "the subject is the installer, by its SHA-256")
        expect({"openssl", "boost", "postgresql", "zlib", "dotnet-windowsdesktop-runtime", "msvc-runtime"} <= set(names),
               "the installer's native components are listed: %s" % names)
        expect("quickjs" not in names and not any(n.startswith("linux-") for n in names),
               "and nothing only the Linux packages carry: %s" % names)
        nuget = dict((c["name"], c) for c in bom["components"] if c["purl"].startswith("pkg:nuget/"))
        expect(sorted(nuget) == ["MimeKit", "SkiaSharp", "WPF-UI"], "every NuGet package of the packed tools, no project: %s" % sorted(nuget))
        expect(nuget.get("WPF-UI", {}).get("hashes") == [{"alg": "SHA-512", "content": sha512.hex()}],
               "a package's SHA-512 is its lock file's contentHash, in hex")
        expect(any(p["value"] == "ControlPanel, ImportTool" for p in nuget.get("WPF-UI", {}).get("properties", [])),
               "a package names every tool that uses it")
        runtime = [c for c in bom["components"] if c["name"] == "msvc-runtime"][0]
        expect(runtime.get("components", [{}])[0].get("hashes") == [{"alg": "SHA-256", "content": crt}],
               "each packed file is listed with the SHA-256 the manifest pins")
        dotnet = [c for c in bom["components"] if c["name"] == "dotnet-windowsdesktop-runtime"][0]
        expect(dotnet.get("licenses") == [{"license": {"name": "Microsoft terms"}}], "a licence without an SPDX id by its name")
        expect(bom["metadata"]["component"]["components"][0]["name"] == "hMailServer.exe", "a first-party file named with --contains")
        expect(structure_problems(bom) == [], "the document is well formed: %s" % structure_problems(bom))
        again = make("windows-x64", installer, dependencies, root=work, commit="c" * 40, contains=[("hMailServer.exe", "d" * 64)])
        held = os.environ.get("SOURCE_DATE_EPOCH")
        os.environ["SOURCE_DATE_EPOCH"] = "1790000000"
        try:
            first = json.dumps(make("windows-x64", installer, dependencies, root=work, commit="c" * 40), sort_keys=True)
            second = json.dumps(make("windows-x64", installer, dependencies, root=work, commit="c" * 40), sort_keys=True)
        finally:
            if held is None:
                del os.environ["SOURCE_DATE_EPOCH"]
            else:
                os.environ["SOURCE_DATE_EPOCH"] = held
        expect(first == second and again["serialNumber"] == bom["serialNumber"], "the same inputs make the same document")
        arm = make("windows-arm64", installer, [dict(d, id="dotnet-windowsdesktop-runtime-arm64", name="dotnet-arm64")
                                                if d["id"] == "dotnet-windowsdesktop-runtime" else d for d in dependencies], root=work)
        expect(sorted(c["name"] for c in arm["components"] if c["purl"].startswith("pkg:nuget/")) == ["SkiaSharp", "WPF-UI"],
               "the ARM64 installer's packages are those of the tools its own script packs")

        # verify: the document and the file.
        sbom_path = write("out/hMailServer-6.3.5-x64.exe" + SUFFIX, json.dumps(bom))
        lines = []
        expect(verify(sbom_path, installer, lines.append) == 0, "verify takes the installer's own document: %s" % lines)
        lines = []
        expect(verify(sbom_path, other, lines.append) == 1 and any("SHA-256" in l for l in lines),
               "verify refuses a document for another file: %s" % lines)
        broken = json.loads(json.dumps(bom))
        broken["components"][1]["bom-ref"] = broken["components"][0]["bom-ref"]
        expect(any("twice" in p for p in structure_problems(broken)), "a bom-ref used twice is caught")
        broken = json.loads(json.dumps(bom))
        broken["dependencies"][0]["dependsOn"].append("nothing")
        expect(any("nothing" in p for p in structure_problems(broken)), "a dependency on no component is caught")
        broken = json.loads(json.dumps(bom))
        broken["components"][0]["hashes"] = [{"alg": "SHA-256", "content": "abc"}]
        expect(any("well-formed" in p for p in structure_problems(broken)), "a malformed hash is caught")

        # What is refused.
        refused("a list without OpenSSL", "no openssl", lambda: make("windows-x64", installer, [d for d in dependencies if d["id"] != "openssl"], root=work))
        refused("a list made before targets existed", "no targets", lambda: make("windows-x64", installer,
                                                                               [dict((k, v) for k, v in d.items() if k != "targets") for d in dependencies], root=work))
        refused("an empty asset", "empty", lambda: make("windows-x64", write("out/empty.exe", b""), dependencies, root=work))
        refused("a kind that is not one", "not a kind", lambda: make("msi", installer, dependencies, root=work))
        os.remove(os.path.join(work, TOOLS, "ImportTool", "packages.lock.json"))
        refused("a packed tool without a lock file", "ImportTool", lambda: make("windows-x64", installer, dependencies, root=work))
        write(os.path.join(TOOLS, "ImportTool", "packages.lock.json"), json.dumps({"version": 1, "dependencies": {"net10.0": {
            "Bad": {"type": "Direct", "resolved": "1.0.0", "contentHash": "not base64!"}}}}))
        refused("a contentHash that is not base64", "not base64", lambda: make("windows-x64", installer, dependencies, root=work))
        write(os.path.join(INSTALLATION, "section_files_64.iss"), '[Files]\nSource: "Tools\\*"\n')
        refused("an installer script that packs no tool it can find", "packs no .NET tool", lambda: make("windows-x64", installer, dependencies, root=work))

        # Linux, with dpkg and rpm answering as the build container's would.
        deb = write("out/hmailserver_6.3.5_amd64.deb", b"!<arch>\na package")
        rpm = write("out/hmailserver-6.3.5-1.x86_64.rpm", b"\xed\xab\xee\xdb a package")
        boost_header = write("boost/version.hpp", "#define BOOST_VERSION 108300\n#define BOOST_LIB_VERSION \"1_83\"\n")
        answers = {
            "libc6-dev": "ii libc6-dev 2.42-0ubuntu3\n",
            "libstdc++-*-dev": "un libstdc++-13-dev <none>\nii libstdc++-15-dev 15.2.0-4ubuntu1\nii libstdc++-9-dev 9.5\n",
            "libssl-dev": "ii libssl-dev 3.5.3-1ubuntu1\n", "libboost-dev": "ii libboost-dev 1.83.0.2ubuntu1\n",
            "libpq-dev": "ii libpq-dev 18.0-1\n",
        }

        def container(arguments):
            if arguments[0] == "dpkg-query":
                return 0, answers.get(arguments[-1], ""), ""
            if arguments[0] == "dpkg-deb":
                return 0, "Version: 6.3.5-1~debian13\nDepends: libc6 (>= 2.38), libssl3t64 (>= 3.0.0),\n libpq5\n", ""
            if arguments[0] == "rpm" and "--queryformat" in arguments:
                return 0, "6.3.5-1\n", ""
            if arguments[0] == "rpm":
                return 0, "/bin/sh\nlibc.so.6()(64bit)\n/bin/sh\nlibssl.so.3()(64bit)\n", ""
            return 127, "", "not here"

        linux = make("deb", deb, dependencies, root=work, commit="c" * 40, query_dpkg=True, read_package=True, run=container,
                     boost_header=boost_header)
        by_name = dict((c["name"], c) for c in linux["components"])
        expect(set(by_name) == {"quickjs", "linux-glibc", "linux-openssl", "linux-boost", "linux-libpq", "linux-libstdcxx"},
               "the .deb lists its own components and nothing only the installer carries: %s" % sorted(by_name))
        expect(by_name.get("linux-boost", {}).get("version") == "1.83.0", "Boost, in the binary, at the version of its header")
        built = dict((c["name"], [p["value"] for p in c["properties"] if p["name"] == "hmailserver:built-against"]) for c in linux["components"])
        expect(built.get("linux-openssl") == ["libssl-dev 3.5.3-1ubuntu1"] and built.get("linux-libstdcxx") == ["libstdc++-15-dev 15.2.0-4ubuntu1"],
               "each library names the package it was built against, the highest where several match: %s" % built)
        declared = [p["value"] for p in linux["metadata"]["component"]["properties"] if p["name"] == "hmailserver:declared-dependency"]
        expect(declared == ["libc6 (>= 2.38)", "libssl3t64 (>= 3.0.0)", "libpq5"], "the .deb's Depends, item by item: %s" % declared)
        expect(linux["metadata"]["component"]["purl"] == "pkg:deb/hmailserver@6.3.5-1~debian13?arch=amd64",
               "the .deb's purl, at the package's own version: %s" % linux["metadata"]["component"]["purl"])
        red = make("rpm", rpm, dependencies, root=work, read_package=True, run=container)
        expect(red["metadata"]["component"]["purl"] == "pkg:rpm/hmailserver@6.3.5-1?arch=x86_64", "the .rpm's purl")
        expect([p["value"] for p in red["metadata"]["component"]["properties"] if p["name"] == "hmailserver:declared-dependency"]
               == ["/bin/sh", "libc.so.6()(64bit)", "libssl.so.3()(64bit)"], "the .rpm's Requires, each once")
        answers.pop("libpq-dev")
        refused("a library dpkg has no package of", "libpq-dev", lambda: make("deb", deb, dependencies, root=work, query_dpkg=True,
                                                                             run=container, boost_header=boost_header))
        refused("dpkg asked about an installer", "for a .deb or an .rpm", lambda: make("windows-x64", installer, dependencies, root=work, query_dpkg=True))

        # A package job's record: the arm64 .deb cross-compiled in Ubuntu 26.04's own amd64
        # container, whose dpkg holds the amd64 host's packages, the arm64 -dev packages
        # and the cross toolchain's, and a Fedora 44 aarch64 sysroot's rpm database.
        # dpkg-query writes "ii " and a space, and a package removed leaves "rc".
        dpkg_record = write("record/dpkg.txt", "\n".join([
            "ii  libc6 amd64 2.42-0ubuntu3", "ii  libc6-dev arm64 2.42-0ubuntu3",
            "ii  libc6-dev-arm64-cross all 2.42-0ubuntu3cross1",
            "ii  libstdc++-15-dev-arm64-cross all 15.2.0-4ubuntu1cross1", "ii  libstdc++-14-dev-arm64-cross all 14.3.0-1cross1",
            "ii  libstdc++6 arm64 15.2.0-4ubuntu1", "ii  libssl-dev amd64 3.5.5-1ubuntu9", "ii  libssl-dev arm64 3.5.5-1ubuntu1",
            "ii  zlib1g-dev arm64 1:1.3.dfsg+really1.3.1-1ubuntu2", "ii  libboost-thread-dev arm64 1.90.0.1ubuntu1",
            "ii  libboost-thread1.90-dev arm64 1.90.0-3ubuntu1", "ii  libpq-dev arm64 18.0-1", "ii  libldap-dev arm64 2.6.10+dfsg-1ubuntu2",
            "rc  libpq-dev amd64 17.5-1", ""]))
        record_header = write("record/boost-version.hpp", "#define BOOST_VERSION 109000\n#define BOOST_LIB_VERSION \"1_90\"\n")
        arm_deb = write("out/hmailserver_6.3.6_arm64.deb", b"!<arch>\nan arm64 package")
        arm = make("deb", arm_deb, dependencies, root=work, commit="c" * 40, dpkg_list=dpkg_record, target_arch="arm64",
                   boost_header=record_header, read_package=True, run=container)
        built = dict((c["name"], [p["value"] for p in c["properties"] if p["name"] == "hmailserver:built-against"]) for c in arm["components"])
        expect(built.get("linux-glibc") == ["libc6-dev-arm64-cross 2.42-0ubuntu3cross1"]
               and built.get("linux-libstdcxx") == ["libstdc++-15-dev-arm64-cross 15.2.0-4ubuntu1cross1"],
               "a cross build's C and C++ libraries are the cross toolchain's, the highest where several match: %s" % built)
        expect(built.get("linux-openssl") == ["libssl-dev:arm64 3.5.5-1ubuntu1"] and built.get("linux-libpq") == ["libpq-dev:arm64 18.0-1"],
               "every other library is the target architecture's -dev package, never the build container's own: %s" % built)
        expect(built.get("linux-boost") == ["libboost-thread-dev:arm64 1.90.0.1ubuntu1"]
               and dict((c["name"], c.get("version")) for c in arm["components"]).get("linux-boost") == "1.90.0",
               "Boost by the package the build installed, at the version of the header the job kept: %s" % built)
        expect(arm["metadata"]["component"]["purl"] == "pkg:deb/hmailserver@6.3.5-1~debian13?arch=arm64",
               "the arm64 .deb's purl names its architecture: %s" % arm["metadata"]["component"]["purl"])
        expect(structure_problems(arm) == [], "the arm64 .deb's document is well formed: %s" % structure_problems(arm))
        # Without the cross toolchain (a native build's record), the target's own -dev package.
        native_record = write("record/native.txt", "ii  libc6-dev amd64 2.41-12\nii  libstdc++-14-dev amd64 14.2.0-19\n"
                              "ii  libssl-dev amd64 3.5.1-1\nii  zlib1g-dev amd64 1:1.3.dfsg-3.1\nii  libboost-dev amd64 1.83.0.2\n"
                              "ii  libpq-dev amd64 17.5-1\nii  libldap-dev amd64 2.6.10+dfsg-1\n")
        native = make("deb", deb, dependencies, root=work, dpkg_list=native_record, target_arch="amd64", boost_header=boost_header)
        expect([p["value"] for c in native["components"] if c["name"] == "linux-glibc" for p in c["properties"]
                if p["name"] == "hmailserver:built-against"] == ["libc6-dev:amd64 2.41-12"],
               "a native build's C library is its own architecture's libc6-dev")
        refused("a record without the target's libpq, only the build container's", "libpq-dev:arm64",
                lambda: make("deb", arm_deb, dependencies, root=work, target_arch="arm64", boost_header=record_header,
                             dpkg_list=write("record/nolibpq.txt", read_text(dpkg_record).replace("ii  libpq-dev arm64", "ii  libpq-dev amd64")),
                             run=container))
        refused("a record with nothing installed", "records no installed package",
                lambda: make("deb", arm_deb, dependencies, root=work, target_arch="arm64", dpkg_list=write("record/empty.txt", "rc  libpq-dev amd64 17.5-1\n")))
        refused("a record that is not there", "cannot be read",
                lambda: make("deb", arm_deb, dependencies, root=work, target_arch="arm64", dpkg_list=os.path.join(work, "record", "none.txt")))
        refused("a record without the target's architecture", "--target-arch",
                lambda: make("deb", arm_deb, dependencies, root=work, dpkg_list=dpkg_record))
        refused("an architecture without a record", "--target-arch is for --dpkg-list",
                lambda: make("deb", arm_deb, dependencies, root=work, target_arch="arm64", query_dpkg=True, run=container))
        refused("a record and dpkg asked as well", "give one",
                lambda: make("deb", arm_deb, dependencies, root=work, dpkg_list=dpkg_record, target_arch="arm64", query_dpkg=True))
        refused("a record for an installer", "for a .deb or an .rpm",
                lambda: make("windows-x64", installer, dependencies, root=work, dpkg_list=dpkg_record, target_arch="arm64"))
        refused("a Boost header the job did not keep", "cannot be read",
                lambda: make("deb", arm_deb, dependencies, root=work, dpkg_list=dpkg_record, target_arch="arm64",
                             boost_header=os.path.join(work, "record", "no-boost.hpp")))
        # rpm -qa, one line per thing a package provides: Fedora's zlib-devel is zlib-ng-compat-devel's.
        rpm_record = write("record/rpm.txt", "\n".join([
            "glibc-devel aarch64 2.42-4.fc44 glibc-devel", "glibc-devel aarch64 2.42-4.fc44 glibc-devel(aarch-64)",
            "libstdc++-devel aarch64 15.2.1-3.fc44 libstdc++-devel", "openssl-devel aarch64 3.5.1-3.fc44 openssl-devel",
            "openssl-devel aarch64 3.5.1-3.fc44 pkgconfig(openssl)",
            "zlib-ng-compat-devel aarch64 2.2.5-2.fc44 zlib-devel", "zlib-ng-compat-devel aarch64 2.2.5-2.fc44 zlib-ng-compat-devel",
            "boost-devel aarch64 1.90.0-2.fc44 boost-devel", "libpq-devel aarch64 18.0-1.fc44 libpq-devel",
            "openldap-devel aarch64 2.6.10-3.fc44 openldap-devel", "gpg-pubkey (none) 105ef944-65ca83d1 gpg-pubkey", ""]))
        arm_rpm = write("out/hmailserver-6.3.6-1.aarch64.rpm", b"\xed\xab\xee\xdb an aarch64 package")
        with_zlib = dependencies + [dep("linux-zlib", "system", ["linux"], "", linkage="dynamic", license="Zlib")]
        red = make("rpm", arm_rpm, with_zlib, root=work, rpm_list=rpm_record, boost_header=record_header, read_package=True, run=container)
        built = dict((c["name"], [p["value"] for p in c["properties"] if p["name"] == "hmailserver:built-against"]) for c in red["components"])
        expect(built.get("linux-zlib") == ["zlib-ng-compat-devel 2.2.5-2.fc44"] and built.get("linux-openssl") == ["openssl-devel 3.5.1-3.fc44"]
               and built.get("linux-boost") == ["boost-devel 1.90.0-2.fc44"],
               "an .rpm's libraries by the package that provides each -devel: %s" % built)
        expect(red["metadata"]["component"]["purl"] == "pkg:rpm/hmailserver@6.3.5-1?arch=aarch64", "the aarch64 .rpm's purl")
        refused("an rpm record without libpq-devel", "libpq-devel, postgresql-devel",
                lambda: make("rpm", arm_rpm, dependencies, root=work, boost_header=record_header,
                             rpm_list=write("record/nolibpq.rpm.txt", read_text(rpm_record).replace("libpq-devel aarch64 18.0-1.fc44 libpq-devel\n", ""))))
        refused("an rpm record and a dpkg record", "give one",
                lambda: make("rpm", arm_rpm, dependencies, root=work, rpm_list=rpm_record, dpkg_list=dpkg_record, target_arch="arm64"))
        # Boost built from boost.org's tarball in the job (EL, openSUSE, Debian 12): no
        # Boost package where the build ran, and the header the job kept says which.
        el_record = write("record/el9.rpm.txt", "\n".join(l for l in read_text(rpm_record).split("\n") if not l.startswith("boost-devel")))
        el_rpm = write("out/hmailserver-6.3.6-1.el9.x86_64.rpm", b"\xed\xab\xee\xdb an el9 package")
        red = make("rpm", el_rpm, with_zlib, root=work, rpm_list=el_record, boost_header=record_header, read_package=True, run=container)
        boost = [c for c in red["components"] if c["name"] == "linux-boost"][0]
        expect(boost.get("version") == "1.90.0" and any("built from boost.org's release tarball" in p["value"] for p in boost["properties"]),
               "Boost built in the job, with no Boost package where the build ran, is said to be, at its header's version: %s" % boost)
        refused("a record with a Boost development package but not the one the build uses", "boost-devel",
                lambda: make("rpm", el_rpm, with_zlib, root=work, boost_header=record_header,
                             rpm_list=write("record/boost-other.rpm.txt", read_text(el_record) + "boost1.78-devel aarch64 1.78.0-1.el9 boost1.78-devel\n")))
        # openSUSE's build containers: Boost's runtime and licence packages, pulled in by
        # the tools, and Boost built from boost.org's tarball all the same.
        suse_record = write("record/leap16.rpm.txt", read_text(el_record)
                            + "libboost_thread1_86_0 x86_64 1.86.0-160000.2.2 libboost_thread.so.1.86.0()(64bit)\n"
                            + "libboost_thread1_86_0 x86_64 1.86.0-160000.2.2 libboost_thread1_86_0\n"
                            + "boost-license1_86_0 noarch 1.86.0-160000.2.2 boost-license1_86_0\n")
        suse_rpm = write("out/hmailserver-6.3.6-1.suse.lp160.x86_64.rpm", b"\xed\xab\xee\xdb a Leap 16 package")
        red = make("rpm", suse_rpm, with_zlib, root=work, rpm_list=suse_record, boost_header=record_header, read_package=True, run=container)
        boost = [c for c in red["components"] if c["name"] == "linux-boost"][0]
        expect(any("built from boost.org's release tarball" in p["value"] for p in boost["properties"]),
               "Boost built in the job beside Boost's runtime and licence packages is said to be built in the job: %s" % boost)
        refused("an openSUSE record with a Boost headers package but not the one the build uses", "boost-devel",
                lambda: make("rpm", suse_rpm, with_zlib, root=work, boost_header=record_header,
                             rpm_list=write("record/leap16-headers.rpm.txt", read_text(suse_record)
                                            + "libboost_headers1_86_0-devel x86_64 1.86.0-160000.2.2 libboost_headers1_86_0-devel\n")))
        debian12 = write("record/debian12.txt", "ii  libc6-dev amd64 2.36-9\nii  libstdc++-12-dev amd64 12.2.0-14\n"
                         "ii  libssl-dev amd64 3.0.17-1\nii  libpq-dev amd64 15.14-0\nii  libldap-dev amd64 2.5.13+dfsg-5\n")
        deb12 = make("deb", deb, dependencies, root=work, dpkg_list=debian12, target_arch="amd64", boost_header=record_header)
        expect(any("built from boost.org" in p["value"] for c in deb12["components"] if c["name"] == "linux-boost" for p in c["properties"]),
               "Debian 12's Boost, built in the job, is said to be")
        deb12_runtime = make("deb", deb, dependencies, root=work, target_arch="amd64", boost_header=record_header,
                             dpkg_list=write("record/debian12-runtime.txt", read_text(debian12) + "ii  libboost-thread1.74.0 amd64 1.74.0+ds1-21\n"))
        expect(any("built from boost.org" in p["value"] for c in deb12_runtime["components"] if c["name"] == "linux-boost" for p in c["properties"]),
               "a Boost shared library package beside Boost built in the job is not taken for what the build is against")

        # An AppImage: the server's libraries as the build recorded them, and what it
        # carries in usr/lib by package.
        image_file = write("out/hMailServer-6.3.6-x86_64.AppImage", b"\x7fELF an AppImage")
        bundled = write("record/bundled.txt", "libpq.so.5 libpq5:amd64 16.10-0ubuntu0.24.04.1 %s postgresql-16 16.10-0ubuntu0.24.04.1\n"
                        "liblber.so.2 libldap2:amd64 2.6.7+dfsg-1~exp1ubuntu8.2 %s openldap 2.6.7+dfsg-1~exp1ubuntu8.2\n\n"
                        % ("1" * 64, "2" * 64))
        commit_a, commit_b = "a" * 40, "b" * 40
        runtime_record = ("# What build/ci/packages/appimage-runtime.sh linked into runtime-x86_64\nalpine 3.24.2\narch x86_64\n"
                          "linked /usr/lib/crti.o musl-dev 1.2.6-r2 musl %s MIT\n"
                          "linked /usr/lib/libc.a musl-dev 1.2.6-r2 musl %s MIT\n"
                          "linked /usr/lib/libfuse3.a fuse3-static 3.18.3-r0 fuse3 %s GPL-2.0-only AND LGPL-2.1-only\n"
                          "linked /usr/lib/liblzo2.a lzo-dev 2.10-r5 lzo %s GPL-2.0-or-later\n" % (commit_a, commit_a, commit_b, commit_b))
        runtime_linked = write("record/runtime-linked.txt", runtime_record)
        appimage = make("appimage", image_file, dependencies, root=work, commit="c" * 40, dpkg_list=native_record, target_arch="amd64",
                        boost_header=boost_header, bundled=bundled, runtime_linked=runtime_linked)
        carried = dict((c["bom-ref"], c) for c in appimage["components"])
        expect(carried.get("bundled-libpq.so.5", {}).get("purl") == "pkg:deb/ubuntu/libpq5@16.10-0ubuntu0.24.04.1?arch=amd64&distro=ubuntu-24.04"
               and "bundled-liblber.so.2" in carried and "native-linux-openssl" in carried and "native-quickjs" in carried,
               "the AppImage lists its bundled libraries by package, beside the server's own: %s" % sorted(carried))
        expect({"name": "hmailserver:source-package", "value": "postgresql-16 16.10-0ubuntu0.24.04.1"}
               in carried.get("bundled-libpq.so.5", {}).get("properties", []),
               "a bundled library names the source package and version it was built from")
        expect(not any(h for c in appimage["components"] if c["bom-ref"].startswith("bundled-") for h in c.get("hashes", [])),
               "a bundled copy, whose RUNPATH the packing changed, is not given the package file's hash as its own")
        musl = carried.get("runtime-musl-dev", {})
        expect(musl.get("purl") == "pkg:apk/alpine/musl-dev@1.2.6-r2?arch=x86_64&distro=alpine-3.24.2"
               and [p["value"] for p in musl.get("properties", []) if p["name"] == "hmailserver:linked-file"] == ["/usr/lib/crti.o", "/usr/lib/libc.a"]
               and {"name": "hmailserver:aports-commit", "value": commit_a} in musl.get("properties", [])
               and carried.get("runtime-fuse3-static", {}).get("licenses") == [{"license": {"name": "GPL-2.0-only AND LGPL-2.1-only"}}]
               and "runtime-lzo-dev" in carried,
               "the AppImage lists what its runtime links statically, by Alpine package, with each file, commit and licence: %s"
               % sorted(r for r in carried if r.startswith("runtime-")))
        expect(appimage["metadata"]["component"]["purl"] == "pkg:generic/hmailserver@6.3.5?file_name=hMailServer-6.3.6-x86_64.AppImage"
               and structure_problems(appimage) == [], "the AppImage's document is well formed: %s" % structure_problems(appimage))
        refused("an AppImage without its bundled list", "--bundled",
                lambda: make("appimage", image_file, dependencies, root=work, dpkg_list=native_record, target_arch="amd64", boost_header=boost_header,
                             runtime_linked=runtime_linked))
        refused("an AppImage without its runtime's record", "--runtime-linked",
                lambda: make("appimage", image_file, dependencies, root=work, dpkg_list=native_record, target_arch="amd64", boost_header=boost_header,
                             bundled=bundled))
        refused("an AppImage read as a package", "no package metadata",
                lambda: make("appimage", image_file, dependencies, root=work, dpkg_list=native_record, target_arch="amd64",
                             boost_header=boost_header, bundled=bundled, runtime_linked=runtime_linked, read_package=True, run=container))
        refused("a bundled library dpkg could not name", "is not '<soname>",
                lambda: make("appimage", image_file, dependencies, root=work, dpkg_list=native_record, target_arch="amd64",
                             boost_header=boost_header, runtime_linked=runtime_linked,
                             bundled=write("record/unknown.txt", "libpq.so.5 unknown unknown %s postgresql-16 16\n" % ("1" * 64))))
        refused("a bundled library without its source package", "is not '<soname>",
                lambda: make("appimage", image_file, dependencies, root=work, dpkg_list=native_record, target_arch="amd64",
                             boost_header=boost_header, runtime_linked=runtime_linked,
                             bundled=write("record/nosource.txt", "libpq.so.5 libpq5:amd64 16.10 %s\n" % ("1" * 64))))
        refused("a bundled library of the other architecture", "not of amd64",
                lambda: make("appimage", image_file, dependencies, root=work, dpkg_list=native_record, target_arch="amd64",
                             boost_header=boost_header, runtime_linked=runtime_linked,
                             bundled=write("record/arm.txt", "libpq.so.5 libpq5:arm64 16 %s postgresql-16 16\n" % ("1" * 64))))
        refused("the runtime's record of the other architecture", "not of the x86_64 AppImage's",
                lambda: make("appimage", image_file, dependencies, root=work, dpkg_list=native_record, target_arch="amd64",
                             boost_header=boost_header, bundled=bundled,
                             runtime_linked=write("record/runtime-arm.txt", runtime_record.replace("arch x86_64", "arch aarch64"))))
        refused("a runtime's record without libfuse3", "records no link of libfuse3",
                lambda: make("appimage", image_file, dependencies, root=work, dpkg_list=native_record, target_arch="amd64",
                             boost_header=boost_header, bundled=bundled,
                             runtime_linked=write("record/runtime-nofuse.txt", "\n".join(
                                 l for l in runtime_record.split("\n") if " fuse3 " not in l))))
        refused("a runtime's record line it cannot read", "is not 'alpine <release>'",
                lambda: make("appimage", image_file, dependencies, root=work, dpkg_list=native_record, target_arch="amd64",
                             boost_header=boost_header, bundled=bundled,
                             runtime_linked=write("record/runtime-bad.txt", runtime_record + "linked /usr/lib/libz.a zlib-static 1.3.2-r0 zlib nocommit Zlib\n")))
        refused("a bundled list for a .deb", "--bundled is for an AppImage",
                lambda: make("deb", deb, dependencies, root=work, dpkg_list=native_record, target_arch="amd64", boost_header=boost_header,
                             bundled=bundled))
        refused("a runtime's record for a .deb", "--runtime-linked is for an AppImage",
                lambda: make("deb", deb, dependencies, root=work, dpkg_list=native_record, target_arch="amd64", boost_header=boost_header,
                             runtime_linked=runtime_linked))

        # The source tarball: what docs/SourceTarball.md declares, each held to the tarball.
        def tarball(path, version, leave_out=None, stray=None):
            with tarfile.open(path, "w:gz") as archive:
                entries = [QUICKJS_VERSION_FILE] + [p if "." in os.path.basename(p) else p + "/x.c" for _, _, p, _, _, _ in SOURCE_COMPONENTS]
                for relative in entries:
                    if leave_out and relative.startswith(leave_out):
                        continue
                    data = b"2025-04-26\n" if relative == QUICKJS_VERSION_FILE else b"text\n"
                    info = tarfile.TarInfo("hmailserver-%s/%s" % (version, relative))
                    info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
                if stray:
                    info = tarfile.TarInfo(stray)
                    archive.addfile(info, io.BytesIO(b""))
            return path

        source = make("source", tarball(os.path.join(work, "out", "hmailserver-6.3.6.tar.gz"), "6.3.6"), [], root=work,
                      version="6.3.6", commit="c" * 40)
        by_ref = dict((c["bom-ref"], c) for c in source["components"])
        expect(sorted(by_ref) == sorted("source-" + ref for ref, _, _, _, _, _ in SOURCE_COMPONENTS)
               and by_ref["source-quickjs"].get("version") == "2025-04-26" and by_ref["source-public-suffix-list"]["licenses"] == [{"license": {"id": "MPL-2.0"}}],
               "the tarball's document lists every declared third-party part, QuickJS at its VERSION: %s" % sorted(by_ref))
        expect(structure_problems(source) == [] and source["metadata"]["component"]["hashes"][0]["content"]
               == digests(os.path.join(work, "out", "hmailserver-6.3.6.tar.gz"))[0], "the tarball's document is well formed and names it")
        refused("a tarball without a declared part", "does not hold hmailserver/source/Server/Common/Mime",
                lambda: make("source", tarball(os.path.join(work, "out", "short.tar.gz"), "6.3.6", leave_out="hmailserver/source/Server/Common/Mime"),
                             [], root=work, version="6.3.6"))
        refused("a tarball of another version", "outside hmailserver-6.3.6/",
                lambda: make("source", tarball(os.path.join(work, "out", "other.tar.gz"), "6.3.5"), [], root=work, version="6.3.6"))
        refused("a tarball with a path outside its top directory", "outside hmailserver-6.3.6/",
                lambda: make("source", tarball(os.path.join(work, "out", "stray.tar.gz"), "6.3.6", stray="elsewhere/x"), [], root=work, version="6.3.6"))
        refused("a file that is not a tarball", "cannot be read as a gzip tarball",
                lambda: make("source", write("out/not.tar.gz", b"not a tarball"), [], root=work, version="6.3.6"))

        # The image: Syft's document and the .deb's.
        syft = {"bomFormat": "CycloneDX", "specVersion": "1.6", "components": [
            {"bom-ref": "pkg:deb/ubuntu/libssl3t64@3.5.3", "type": "library", "name": "libssl3t64", "version": "3.5.3"},
            {"bom-ref": "pkg:deb/hmailserver@6.3.5", "type": "library", "name": "hmailserver", "version": "6.3.5"}],
            "metadata": {"tools": {"components": [{"type": "application", "author": "anchore", "name": "syft", "version": "1.33.0"}]}}}
        digest = "e" * 64
        image = image_document(syft, linux, "registry.gitlab.com/hmailserver/hmailserver@sha256:" + digest, "6.3.5", "amd64")
        names = [c["name"] for c in image["components"]]
        expect("libssl3t64" in names and "quickjs" in names and "linux-boost" in names, "the image lists Syft's packages and the .deb's own: %s" % names)
        expect(image["metadata"]["component"]["hashes"] == [{"alg": "SHA-256", "content": digest}] and
               image["metadata"]["component"]["type"] == "container", "the subject is the image, by its digest")
        expect(image["metadata"]["component"]["purl"] == "pkg:oci/hmailserver@sha256%3A" + digest
               + "?arch=amd64&repository_url=registry.gitlab.com/hmailserver/hmailserver"
               and {"name": "hmailserver:platform", "value": "linux/amd64"} in image["metadata"]["component"]["properties"],
               "the subject names its platform: %s" % image["metadata"]["component"]["purl"])
        expect(structure_problems(image) == [], "the image's document is well formed: %s" % structure_problems(image))
        arm_image = image_document(syft, arm, "registry.gitlab.com/hmailserver/hmailserver@sha256:" + "f" * 64, "6.3.6", "arm64")
        expect("?arch=arm64&" in arm_image["metadata"]["component"]["purl"] and "linux/arm64" in arm_image["metadata"]["component"]["description"]
               and structure_problems(arm_image) == [], "the arm64 image's document names linux/arm64: %s" % arm_image["metadata"]["component"])
        refused("an image named by a tag", "by its digest", lambda: image_document(syft, linux, "registry.gitlab.com/x/y:6.3.5", "6.3.5", "amd64"))
        refused("an installer's document as the .deb's", "not a .deb", lambda: image_document(syft, bom, "r.example/x@sha256:" + digest, "6.3.5", "amd64"))
        refused("the amd64 .deb's document for the arm64 image", "not the arm64 .deb",
                lambda: image_document(syft, linux, "r.example/x@sha256:" + digest, "6.3.5", "arm64"))
        refused("the arm64 .deb's document for the amd64 image", "not the amd64 .deb",
                lambda: image_document(syft, arm, "r.example/x@sha256:" + digest, "6.3.6", "amd64"))
        refused("an image of a platform not built", "built for amd64 and arm64",
                lambda: image_document(syft, linux, "r.example/x@sha256:" + digest, "6.3.5", "riscv64"))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


# ---------------------------------------------------------------- main

def write_document(bom, path):
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(bom, f, indent=2, ensure_ascii=False)
        f.write("\n")


def main():
    if "--self-test" in sys.argv[1:]:
        return self_test()
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    commands = parser.add_subparsers(dest="command")
    maker = commands.add_parser("make", help="the SBOM of one installer or package")
    maker.add_argument("--kind", required=True, choices=sorted(KINDS))
    maker.add_argument("--asset", required=True)
    maker.add_argument("--dependencies", help="build/native-dependencies.ps1's output (-Platform ARM64 for windows-arm64); "
                       "every kind but source")
    maker.add_argument("--out")
    maker.add_argument("--root", default=ROOT, help="the tree the asset was built from (lock files, installer script, Version.h)")
    maker.add_argument("--version")
    maker.add_argument("--commit")
    maker.add_argument("--contains", action="append", default=[], metavar="NAME=SHA256",
                       help="a file of this project's own build the asset carries, by its SHA-256")
    maker.add_argument("--query-dpkg", action="store_true", help="in the build container: what each library was built against")
    maker.add_argument("--dpkg-list", help="what the build container's dpkg had installed, as a package job recorded it "
                       "(built-against/dpkg.txt): what each library was built against, read in place of --query-dpkg")
    maker.add_argument("--rpm-list", help="what the build's rpm database held, as a package job recorded it "
                       "(built-against/rpm.txt): what each library was built against")
    maker.add_argument("--target-arch", help="with --dpkg-list: the package's own architecture as dpkg names it")
    maker.add_argument("--boost-header", default=BOOST_HEADER,
                       help="Boost's version.hpp as the build compiled against it (%s)" % BOOST_HEADER)
    maker.add_argument("--read-package", action="store_true", help="what the .deb or the .rpm declares it depends on")
    maker.add_argument("--bundled", help="an AppImage's bundled.txt (build/ci/packages/appimage-build.sh): the libraries "
                       "it carries, by package and source package")
    maker.add_argument("--runtime-linked", help="an AppImage's runtime-linked.txt (build/ci/packages/appimage-build.sh keeps "
                       "the runtime job's record): what its runtime links statically, by Alpine package")
    imager = commands.add_parser("image", help="the SBOM of one platform's container image")
    imager.add_argument("--syft", required=True, help="Syft's CycloneDX JSON of the image")
    imager.add_argument("--deb-sbom", required=True, help="the SBOM of that architecture's .deb, which this script made")
    imager.add_argument("--image", required=True, help="<repository>@sha256:<the platform's manifest digest>")
    imager.add_argument("--arch", required=True, choices=IMAGE_ARCHES, help="the image's architecture: linux/<arch>")
    imager.add_argument("--version", required=True)
    imager.add_argument("--out", required=True)
    checker = commands.add_parser("verify", help="the SBOM describes this very file")
    checker.add_argument("sbom")
    checker.add_argument("asset")
    args = parser.parse_args()
    try:
        if args.command == "make":
            contains = []
            for item in args.contains:
                name, _, digest = item.partition("=")
                if not name or not re.match(r"^[0-9a-f]{64}$", digest):
                    raise SbomError("--contains takes NAME=<SHA-256 in hex>, not %r" % item)
                contains.append((name, digest))
            if args.kind == "source":
                if args.dependencies:
                    raise SbomError("a source tarball's document is read from the tarball alone: no --dependencies")
                dependencies = []
            else:
                if not args.dependencies:
                    raise SbomError("--dependencies is required for a %s" % args.kind)
                dependencies = read_json(args.dependencies)
                if not isinstance(dependencies, list):
                    raise SbomError("%s is not a list of components" % args.dependencies)
            bom = make(args.kind, args.asset, dependencies, root=args.root, version=args.version, commit=args.commit,
                       contains=contains, query_dpkg=args.query_dpkg, read_package=args.read_package,
                       boost_header=args.boost_header, dpkg_list=args.dpkg_list, rpm_list=args.rpm_list,
                       target_arch=args.target_arch, bundled=args.bundled, runtime_linked=args.runtime_linked)
            out = args.out or args.asset + SUFFIX
            write_document(bom, out)
            print("%s: %d components, describing %s (SHA-256 %s)" % (out, len(bom["components"]), os.path.basename(args.asset),
                                                                      bom["metadata"]["component"]["hashes"][0]["content"]))
            return 0
        if args.command == "image":
            bom = image_document(read_json(args.syft), read_json(args.deb_sbom), args.image, args.version, args.arch)
            write_document(bom, args.out)
            print("%s: %d components, describing %s" % (args.out, len(bom["components"]), args.image))
            return 0
        if args.command == "verify":
            return verify(args.sbom, args.asset)
    except SbomError as e:
        print("REFUSED: %s" % e)
        return 1
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
