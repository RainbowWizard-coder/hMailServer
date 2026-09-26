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
  image
      Syft's CycloneDX document of the pushed image (every distribution package in
      it, the hmailserver .deb among them), with the .deb's own document's
      components added - what Syft cannot see inside the binary - and the image,
      by its digest, as the subject. The container image's jobs attest it to the
      image with cosign (.gitlab/ci/container-image.yml).

The document is the same for the same inputs: its serial number is made from the
file's SHA-256, and its timestamp is SOURCE_DATE_EPOCH's when that is set.

Usage:
  python3 build/asset-sbom.py make --kind windows-x64 --asset hMailServer-6.3.5-x64.exe \\
          --dependencies native-dependencies-x64.json [--out FILE] [--contains NAME=SHA256]...
  python3 build/asset-sbom.py make --kind deb --asset hmailserver_6.3.5_amd64.deb \\
          --dependencies native-dependencies-x64.json --query-dpkg --read-package
  python3 build/asset-sbom.py image --syft image.syft.cdx.json --deb-sbom hmailserver_6.3.5_amd64.deb.cdx.json \\
          --image registry.gitlab.com/progressiverobot/hmailserver@sha256:<digest> --version 6.3.5 --out FILE
  python3 build/asset-sbom.py verify FILE.cdx.json FILE      the document describes this very file
  python3 build/asset-sbom.py --self-test                    no network, no dpkg, no rpm

The default --out is the asset's own path with .cdx.json added. How a user finds and
checks these documents: SECURITY.md, "Software bills of materials".
"""

import argparse
import base64
import datetime
import glob
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = "1.6"
SUFFIX = ".cdx.json"
PROJECT_URL = "https://gitlab.com/Progressiverobot/hmailserver"
SUPPLIER = {"name": "Progressive Robot Ltd", "url": ["https://www.progressiverobot.com"]}
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
}

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
             "deb": "the Debian and Ubuntu package", "rpm": "the RPM package", "image": "the container image"}
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
        "description": "%s, %s: %s" % (asset_name, names.get(kind, kind), "hMailServer, a mail server for SMTP, IMAP and POP3"),
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
         read_package=False, run=default_run, boost_header=BOOST_HEADER):
    if kind not in KINDS:
        raise SbomError("%s is not a kind of asset this makes (%s)" % (kind, ", ".join(sorted(KINDS))))
    if not os.path.isfile(asset) or os.path.getsize(asset) == 0:
        raise SbomError("%s is not a file, or is empty" % asset)
    if (query_dpkg or read_package) and KINDS[kind]["target"] != "linux":
        raise SbomError("--query-dpkg and --read-package are for a .deb or an .rpm")
    version = version or read_version(root)
    sha256, sha512, size = digests(asset)
    components = native_components(dependencies, kind)
    if KINDS[kind]["target"] == "windows":
        components += nuget_components(root, kind)
    if query_dpkg:
        dpkg_built_against([c for c in components if c["bom-ref"].startswith("native-linux-")], run, boost_header)
    package_version, declared = package_facts(kind, asset, run) if read_package else (None, None)
    component = subject(os.path.basename(asset), kind, version, sha256, sha512, size,
                        commit if commit is not None else commit_of(root), declared, contains, package_version)
    return document(component, components, sha256, kind)


def image_document(syft, deb_sbom, image, version):
    """The pushed image's document: Syft's components, the .deb's own added."""
    match = re.match(r"^([a-z0-9.\-]+(?::[0-9]+)?/[a-z0-9._/\-]+)@sha256:([0-9a-f]{64})$", image or "")
    if not match:
        raise SbomError("the image is %r, not <repository>@sha256:<digest>: an image is described by its digest" % image)
    if syft.get("bomFormat") != "CycloneDX":
        raise SbomError("Syft's document is not CycloneDX")
    if deb_sbom.get("bomFormat") != "CycloneDX" or (deb_sbom.get("metadata") or {}).get("component", {}).get("properties") is None:
        raise SbomError("the .deb's document is not one this script made")
    kind_of = dict((p["name"], p["value"]) for p in deb_sbom["metadata"]["component"]["properties"])
    if kind_of.get("hmailserver:asset-kind") != "deb":
        raise SbomError("the .deb's document describes a %s, not a .deb" % kind_of.get("hmailserver:asset-kind"))
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
        "description": "The hMailServer %s container image, built from %s" % (version, kind_of.get("hmailserver:asset-file")),
        "hashes": [{"alg": "SHA-256", "content": digest}],
        "licenses": [{"license": {"id": "AGPL-3.0-or-later"}}],
        "purl": "pkg:oci/hmailserver@sha256%%3A%s?repository_url=%s" % (digest, repository),
        "externalReferences": [{"type": "vcs", "url": PROJECT_URL}],
        "properties": [prop("asset-file", image), prop("asset-kind", "image"), prop("built-from", kind_of.get("hmailserver:asset-file"))],
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

        # The image: Syft's document and the .deb's.
        syft = {"bomFormat": "CycloneDX", "specVersion": "1.6", "components": [
            {"bom-ref": "pkg:deb/ubuntu/libssl3t64@3.5.3", "type": "library", "name": "libssl3t64", "version": "3.5.3"},
            {"bom-ref": "pkg:deb/hmailserver@6.3.5", "type": "library", "name": "hmailserver", "version": "6.3.5"}],
            "metadata": {"tools": {"components": [{"type": "application", "author": "anchore", "name": "syft", "version": "1.33.0"}]}}}
        digest = "e" * 64
        image = image_document(syft, linux, "registry.gitlab.com/progressiverobot/hmailserver@sha256:" + digest, "6.3.5")
        names = [c["name"] for c in image["components"]]
        expect("libssl3t64" in names and "quickjs" in names and "linux-boost" in names, "the image lists Syft's packages and the .deb's own: %s" % names)
        expect(image["metadata"]["component"]["hashes"] == [{"alg": "SHA-256", "content": digest}] and
               image["metadata"]["component"]["type"] == "container", "the subject is the image, by its digest")
        expect(structure_problems(image) == [], "the image's document is well formed: %s" % structure_problems(image))
        refused("an image named by a tag", "by its digest", lambda: image_document(syft, linux, "registry.gitlab.com/x/y:6.3.5", "6.3.5"))
        refused("an installer's document as the .deb's", "not a .deb", lambda: image_document(syft, bom, "r.example/x@sha256:" + digest, "6.3.5"))
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
    maker.add_argument("--dependencies", required=True, help="build/native-dependencies.ps1's output (-Platform ARM64 for windows-arm64)")
    maker.add_argument("--out")
    maker.add_argument("--root", default=ROOT, help="the tree the asset was built from (lock files, installer script, Version.h)")
    maker.add_argument("--version")
    maker.add_argument("--commit")
    maker.add_argument("--contains", action="append", default=[], metavar="NAME=SHA256",
                       help="a file of this project's own build the asset carries, by its SHA-256")
    maker.add_argument("--query-dpkg", action="store_true", help="in the build container: what each library was built against")
    maker.add_argument("--read-package", action="store_true", help="what the .deb or the .rpm declares it depends on")
    imager = commands.add_parser("image", help="the SBOM of the container image")
    imager.add_argument("--syft", required=True, help="Syft's CycloneDX JSON of the image")
    imager.add_argument("--deb-sbom", required=True, help="the .deb's own SBOM, which this script made")
    imager.add_argument("--image", required=True, help="<repository>@sha256:<digest>")
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
            dependencies = read_json(args.dependencies)
            if not isinstance(dependencies, list):
                raise SbomError("%s is not a list of components" % args.dependencies)
            bom = make(args.kind, args.asset, dependencies, root=args.root, version=args.version, commit=args.commit,
                       contains=contains, query_dpkg=args.query_dpkg, read_package=args.read_package)
            out = args.out or args.asset + SUFFIX
            write_document(bom, out)
            print("%s: %d components, describing %s (SHA-256 %s)" % (out, len(bom["components"]), os.path.basename(args.asset),
                                                                      bom["metadata"]["component"]["hashes"][0]["content"]))
            return 0
        if args.command == "image":
            bom = image_document(read_json(args.syft), read_json(args.deb_sbom), args.image, args.version)
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
