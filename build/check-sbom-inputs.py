#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every pinned build input is named in the SBOM, at the version and with the hash it is pinned by.

Syft inventories the tree - the .NET packages and the files it can see - and
build/merge-native-dependencies-into-sbom.ps1 adds everything else from the list
build/native-dependencies.ps1 makes: the linked and vendored native libraries,
what the Windows installer carries, and the Linux packages' system libraries.
An input the build pins and that list forgets is an SBOM that is silently one
short, which is the failure the list exists to prevent. So this reads what each
of the build's own files pins, independently of native-dependencies.ps1, and
holds the list to it:

  third-party-binaries.json   every artifact of a kind that carries a pin -
                              fetched (the SHA-256 of the build input it comes
                              from), gathered (its version) and downloaded (its
                              SHA-512) - by its component and version. The kinds
                              that carry none are named below with the reason; a
                              kind this file does not know fails.
  native-dependencies.ps1     every entry of its $dependencies table, at the
                              version this file reads itself from the entry's
                              include-path pattern or version file.
  get-installer-binaries.ps1  every kind it places, and every build input its
                              fetched files come from, by hash.
  get-dotnet-runtime.ps1      the entry it pins ($pinnedPath), which must be a
                              pin: one release, a SHA-512, an address naming it.

and, for the Linux packages and the build's other reach:

  CMakeLists.txt              every find_package and find_library; a lookup of a
                              kind this file does not read (pkg_check_modules,
                              FetchContent, ExternalProject) fails.
  the native projects         every directory under $(hMailServerLibs) that
                              hMailServer.vcxproj or a project beside it
                              reaches matches a pattern of native-dependencies.ps1.
  hmailserver/source/Server   every directory with a LICENSE of its own is
                              third-party source, and is a vendored entry.

The component list it holds them to is, in the order it is available:

  --sbom FILE (repeatable)    finished SBOM documents, SPDX or CycloneDX JSON:
                              the components the merge added are found by their
                              identifiers (SPDXRef-Package-native-<id>, bom-ref
                              hmailserver-native-<id>). What the SBOM job runs
                              after the merge, and what anyone can run against a
                              release's SBOM.
  native-dependencies.ps1     run with pwsh or powershell when one is on PATH,
                              and its output checked the same way.
  its declarations            without PowerShell (GitLab's Python image): its
                              tables are read, and every pinned input must have
                              an entry in them. The tables are what the script
                              emits from, and the script throws on an input it
                              has no entry for, so this is the same rule one step
                              earlier.

What it would not notice: an input the build fetches with no pin at all -
hmailserver/docs/ThirdPartyBinaries.md, "Every build input, and its pin",
lists those with their reasons - and a table of native-dependencies.ps1 written
in another shape than the one read here (keep each entry's fields as they are,
or teach this file the new shape in the same change).

Usage:
  python build/check-sbom-inputs.py                    # the tree
  python build/check-sbom-inputs.py --sbom a.spdx.json --sbom b.cyclonedx.json
  python build/check-sbom-inputs.py --self-test        # the negative controls
"""
import copy
import io
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

PATHS = {
    "manifest": "hmailserver/docs/third-party-binaries.json",
    "producer": "build/native-dependencies.ps1",
    "installer_script": "build/get-installer-binaries.ps1",
    "runtime_script": "build/get-dotnet-runtime.ps1",
    "vcxproj": "hmailserver/source/Server/hMailServer/hMailServer.vcxproj",
    "cmake": "hmailserver/source/Server/CMakeLists.txt",
}

# The manifest's kinds, and what each is pinned by. A kind in neither table fails,
# so a new one has to be decided here: pinned, and named in the SBOM, or not, with why.
PINNED = {
    "fetched": "the SHA-256 of the build input it is fetched from",
    "gathered": "its version, against the toolset that built the server",
    "downloaded": "the SHA-512 its vendor publishes",
}
UNPINNED = {
    "system": "Windows' own file, compiled against as the Windows SDK's headers are and never redistributed; its hash is reported, not enforced",
    "generated": "this project's own COM interop assembly, made on every build",
}

SERVER = "hmailserver/source/Server"


def read(path):
    with io.open(path, "r", encoding="utf-8-sig") as f:
        return f.read()


def load(root=ROOT):
    model = {"root": root}
    for key, relative in PATHS.items():
        text = read(os.path.join(root, relative))
        model[key] = json.loads(text) if key == "manifest" else text
    server = os.path.join(root, SERVER)
    # The other native projects beside the server's (the minidump writer, the updater),
    # for the library directories they reach.
    model["other_projects"] = "\n".join(
        read(os.path.join(server, name, project))
        for name in sorted(os.listdir(server)) if os.path.isdir(os.path.join(server, name))
        for project in sorted(os.listdir(os.path.join(server, name)))
        if project.endswith(".vcxproj") and "%s/%s/%s" % (SERVER, name, project) != PATHS["vcxproj"])
    model["licensed_dirs"] = sorted(
        name for name in os.listdir(server)
        if os.path.isdir(os.path.join(server, name)) and os.path.isfile(os.path.join(server, name, "LICENSE")))
    return model


# ------------------------------------------------------------------ PowerShell tables

PS_STRING = r"'((?:[^']|'')*)'"


def ps_unquote(value):
    return value.replace("''", "'")


def ps_field(block, name):
    match = re.search(r"\b" + name + r"\s*=\s*" + PS_STRING, block)
    return ps_unquote(match.group(1)) if match else None


def ps_list(block, name):
    match = re.search(r"\b" + name + r"\s*=\s*@\(([^)]*)\)", block)
    return [ps_unquote(v) for v in re.findall(PS_STRING, match.group(1))] if match else None


def ps_block(text, variable, opener):
    """The body of '$variable = @(' or '$variable = @{' up to its closing line."""
    closer = ")" if opener == "(" else "}"
    match = re.search(r"^\$" + variable + r"\s*=\s*@" + re.escape(opener) + r"(.*?)^" + re.escape(closer), text, re.S | re.M)
    return match.group(1) if match else None


def parse_producer(text):
    producer = {"dependencies": [], "pinned": None, "installer": {}, "linux_map": {}, "linux_always": [], "linux_components": set()}
    block = ps_block(text, "dependencies", "(")
    if block:
        for entry in re.findall(r"@\{(.*?)\n   \}", block, re.S):
            producer["dependencies"].append({
                "name": ps_field(entry, "Name"),
                "pattern": ps_field(entry, "Pattern"),
                "version_file": ps_list(entry, "VersionFile"),
                "version_pattern": ps_field(entry, "VersionPattern"),
                "vendored": ps_field(entry, "Vendored"),
            })
    match = re.search(r"^\$pinnedDispositions\s*=\s*@\(([^)]*)\)", text, re.M)
    producer["pinned"] = [ps_unquote(v) for v in re.findall(PS_STRING, match.group(1))] if match else None
    block = ps_block(text, "installerComponents", "{")
    if block:
        for key, body in re.findall(r"^   " + PS_STRING + r"\s*=\s*@\{(.*?)\n   \}", block, re.S | re.M):
            producer["installer"][ps_unquote(key)] = ps_field(body, "Id")
    block = ps_block(text, "linuxLibraries", "{")
    if block:
        for key, value in re.findall(PS_STRING + r"\s*=\s*" + PS_STRING, block):
            producer["linux_map"][ps_unquote(key)] = ps_unquote(value)
    always = re.search(r"^\$linuxAlways\s*=\s*@\(([^)]*)\)", text, re.M)
    if always:
        producer["linux_always"] = [ps_unquote(v) for v in re.findall(PS_STRING, always.group(1))]
    block = ps_block(text, "linuxComponents", "{")
    if block:
        producer["linux_components"] = set(ps_unquote(k) for k in re.findall(r"^   " + PS_STRING + r"\s*=\s*@\{", block, re.M))
    return producer


# ------------------------------------------------------------------ what is pinned

def resolve_version(model, dependency):
    if dependency["version_file"]:
        path = os.path.join(model["root"], *dependency["version_file"])
        if not os.path.isfile(path):
            return None, "/".join(dependency["version_file"])
        text = read(path)
        if dependency["version_pattern"]:
            match = re.search(dependency["version_pattern"], text)
            return (match.group(1) if match else None), "/".join(dependency["version_file"])
        return (text.strip() or None), "/".join(dependency["version_file"])
    if dependency["pattern"]:
        match = re.search(dependency["pattern"], model["vcxproj"])
        if not match:
            return None, "hMailServer.vcxproj"
        return ".".join(match.groups()), "hMailServer.vcxproj"
    return None, "nothing (the entry has neither Pattern nor VersionFile)"


def analyse(model):
    """(problems, expected) - expected is {id: {what, version, hashes, source}}."""
    problems = []
    expected = {}
    producer = parse_producer(model["producer"])
    manifest = model["manifest"]

    def expect(identifier, what, version, hashes, source):
        entry = expected.setdefault(identifier, {"what": what, "version": version, "hashes": set(), "source": source})
        entry["hashes"].update(hashes)

    # native-dependencies.ps1's own table.
    if not producer["dependencies"]:
        problems.append("native-dependencies.ps1: no $dependencies table was found - its shape changed, and this file must be taught the new one")
    for dependency in producer["dependencies"]:
        if not dependency["name"]:
            problems.append("native-dependencies.ps1: a $dependencies entry has no Name")
            continue
        version, where = resolve_version(model, dependency)
        if not version:
            problems.append("native-dependencies.ps1 names %s, whose version cannot be read from %s" % (dependency["name"], where))
            continue
        expect(dependency["name"], "%s, from %s" % (dependency["name"], where), version, set(), "native-dependencies.ps1")

    # The kinds: known, and the pinned ones all emitted.
    kinds = set(a.get("disposition") for a in manifest.get("artifacts", []))
    installer_kinds = set(re.findall(r"disposition\s+-eq\s+'(\w+)'", model["installer_script"]))
    for kind in sorted(kinds | installer_kinds):
        if kind not in PINNED and kind not in UNPINNED:
            problems.append("the kind '%s' (third-party-binaries.json or get-installer-binaries.ps1) is not one this checker knows: "
                            "decide whether it carries a pin, and add it to PINNED or UNPINNED here" % kind)
    if producer["pinned"] is None:
        problems.append("native-dependencies.ps1: no $pinnedDispositions list was found")
    else:
        for kind in sorted(PINNED):
            if kind not in producer["pinned"]:
                problems.append("native-dependencies.ps1 does not emit the manifest's '%s' entries, which are pinned by %s" % (kind, PINNED[kind]))

    # third-party-binaries.json and get-installer-binaries.ps1: every pinned artifact.
    build_inputs = {}
    for build_input in manifest.get("build_inputs", []):
        build_inputs["%s/%s" % (build_input.get("release"), build_input.get("asset"))] = (build_input.get("sha256") or "").lower()
    for artifact in manifest.get("artifacts", []):
        kind = artifact.get("disposition")
        if kind not in PINNED:
            continue
        component, version = artifact.get("component"), artifact.get("version")
        identifier = producer["installer"].get(component)
        if not identifier:
            problems.append("third-party-binaries.json pins %s %s (%s, %s), which native-dependencies.ps1 has no SBOM entry for "
                            "($installerComponents)" % (component, version, artifact.get("path"), kind))
            continue
        hashes = set()
        if kind == "fetched":
            asset = (artifact.get("source") or "").split("#", 1)[0]
            digest = build_inputs.get(asset)
            if not re.match(r"^[0-9a-f]{64}$", digest or ""):
                problems.append("third-party-binaries.json: %s is fetched from %s, which build_inputs gives no SHA-256 for" % (artifact.get("path"), asset))
            else:
                hashes.add(("SHA-256", digest))
        elif kind == "downloaded":
            digest = (artifact.get("sha512") or "").lower()
            if not re.match(r"^[0-9a-f]{128}$", digest):
                problems.append("third-party-binaries.json: %s is downloaded but carries no SHA-512 to check it against" % artifact.get("path"))
            else:
                hashes.add(("SHA-512", digest))
            if artifact.get("sha256"):
                hashes.add(("SHA-256", artifact["sha256"].lower()))
        expect(identifier, "%s %s" % (component, version), version, hashes, "third-party-binaries.json (%s)" % kind)

    # get-dotnet-runtime.ps1: the entry it places is there, and is a pin.
    match = re.search(r"^\$pinnedPath\s*=\s*" + PS_STRING, model["runtime_script"], re.M)
    if not match:
        problems.append("get-dotnet-runtime.ps1: no $pinnedPath line names the manifest entry it places")
    else:
        path = ps_unquote(match.group(1))
        entries = [a for a in manifest.get("artifacts", []) if a.get("path") == path]
        if len(entries) != 1:
            problems.append("get-dotnet-runtime.ps1 places %s, which third-party-binaries.json has %d entries for (one is needed, the pin)" % (path, len(entries)))
        else:
            entry = entries[0]
            version = entry.get("version") or ""
            source = entry.get("source") or ""
            if entry.get("disposition") != "downloaded":
                problems.append("get-dotnet-runtime.ps1's pin (%s) is of the kind '%s', not 'downloaded'" % (path, entry.get("disposition")))
            if not re.match(r"^\d+\.\d+\.\d+$", version):
                problems.append("get-dotnet-runtime.ps1's pin names no single release (version '%s')" % version)
            if not re.match(r"^https://", source) or not source.endswith("/windowsdesktop-runtime-%s-win-x64.exe" % version) or "aka.ms" in source:
                problems.append("get-dotnet-runtime.ps1's pin downloads from '%s', which is not an https address for release %s alone "
                                "(a moving link such as aka.ms is not a pin)" % (source, version))

    # CMakeLists.txt: every library the Linux build finds.
    cmake = model["cmake"]
    for lookup in ("pkg_check_modules", "pkg_search_module", "FetchContent_Declare", "ExternalProject_Add"):
        if re.search(r"^\s*" + lookup + r"\s*\(", cmake, re.M):
            problems.append("CMakeLists.txt calls %s, a dependency lookup neither native-dependencies.ps1 nor this checker reads; "
                            "teach both, so what it finds is named in the SBOM" % lookup)
    names = re.findall(r"^\s*find_package\(\s*(\w+)", cmake, re.M) + re.findall(r"^\s*find_library\(\s*\w+\s+NAMES\s+(\w+)", cmake, re.M)
    linux = []
    for name in names:
        component = producer["linux_map"].get(name)
        if not component:
            problems.append("CMakeLists.txt finds %s, which native-dependencies.ps1 has no SBOM entry for ($linuxLibraries)" % name)
            continue
        linux.append((component, "found as %s by CMakeLists.txt" % name))
    for component in producer["linux_always"]:
        linux.append((component, "the C++ runtime"))
    for component, why in linux:
        if component not in producer["linux_components"]:
            problems.append("native-dependencies.ps1 maps a Linux library to %s, which $linuxComponents does not describe" % component)
            continue
        expect("linux-" + component, "the Linux packages' %s, %s" % (component, why), "", set(), "CMakeLists.txt")

    # The native projects: every library directory they reach.
    patterns = [d["pattern"] for d in producer["dependencies"] if d["pattern"]]
    projects = model["vcxproj"] + "\n" + model.get("other_projects", "")
    for directory in sorted(set(re.findall(r"\$\(hMailServerLibs\)\\([^\\;<\"]+)", projects))):
        if not any(re.search(p, directory) for p in patterns):
            problems.append("a native project reaches $(hMailServerLibs)\\%s, which no native-dependencies.ps1 pattern names" % directory)

    # hmailserver/source/Server: every third-party source tree.
    vendored = set()
    for dependency in producer["dependencies"]:
        if dependency["vendored"]:
            vendored.add(dependency["vendored"].rstrip("/").split("/")[-1])
    for directory in model["licensed_dirs"]:
        if directory not in vendored:
            problems.append("%s/%s carries a LICENSE of its own - third-party source - and native-dependencies.ps1 has no vendored entry for it" % (SERVER, directory))

    return problems, expected


# ------------------------------------------------------------------ the component list

def normalise_algorithm(name):
    name = (name or "").upper().replace("-", "")
    return {"SHA256": "SHA-256", "SHA512": "SHA-512", "SHA1": "SHA-1"}.get(name, name)


def components_from_list(items):
    """The JSON native-dependencies.ps1 writes."""
    found = {}
    for item in items:
        identifier = item.get("id") or item.get("name")
        found[identifier] = {
            "version": item.get("version") or "",
            "hashes": set((normalise_algorithm(h.get("algorithm")), (h.get("value") or "").lower()) for h in (item.get("hashes") or [])),
        }
    return found


def components_from_document(document):
    """An SPDX or CycloneDX document the merge has added to."""
    found = {}
    if "spdxVersion" in document:
        for package in document.get("packages") or []:
            spdx_id = package.get("SPDXID") or ""
            if spdx_id.startswith("SPDXRef-Package-native-"):
                found[spdx_id[len("SPDXRef-Package-native-"):]] = {
                    "version": package.get("versionInfo") or "",
                    "hashes": set((normalise_algorithm(c.get("algorithm")), (c.get("checksumValue") or "").lower()) for c in package.get("checksums") or []),
                }
        return found
    if document.get("bomFormat") == "CycloneDX":
        for component in document.get("components") or []:
            reference = component.get("bom-ref") or ""
            if reference.startswith("hmailserver-native-"):
                found[reference[len("hmailserver-native-"):]] = {
                    "version": component.get("version") or "",
                    "hashes": set((normalise_algorithm(h.get("alg")), (h.get("content") or "").lower()) for h in component.get("hashes") or []),
                }
        return found
    raise ValueError("neither SPDX (no spdxVersion) nor CycloneDX (no bomFormat)")


def check_components(expected, found, where):
    problems = []
    for identifier in sorted(expected):
        entry = expected[identifier]
        component = found.get(identifier)
        if component is None:
            problems.append("%s names no %s: %s, pinned in %s" % (where, identifier, entry["what"], entry["source"]))
            continue
        if component["version"] != entry["version"]:
            if entry["version"]:
                problems.append("%s gives %s as version '%s'; it is pinned at '%s' (%s)" % (where, identifier, component["version"], entry["version"], entry["source"]))
            else:
                problems.append("%s gives %s a version, '%s', which nothing pins: it is the distribution's" % (where, identifier, component["version"]))
        for algorithm, value in sorted(entry["hashes"]):
            if (algorithm, value) not in component["hashes"]:
                problems.append("%s does not give %s the %s it is pinned by (%s...)" % (where, identifier, algorithm, value[:16]))
    return problems


def run_producer(root=ROOT):
    """native-dependencies.ps1's output, or None when no PowerShell is on PATH."""
    shell = shutil.which("pwsh") or shutil.which("powershell")
    if not shell:
        return None, None
    process = subprocess.run([shell, "-NoProfile", "-File", os.path.join(root, PATHS["producer"])],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=root)
    output = process.stdout.decode("utf-8", "replace").lstrip("\ufeff")
    if process.returncode != 0:
        return None, "native-dependencies.ps1 failed (exit %d): %s" % (process.returncode, (process.stderr.decode("utf-8", "replace") or output).strip()[:800])
    return json.loads(output), os.path.splitext(os.path.basename(shell))[0].lower()


# ------------------------------------------------------------------ the negative controls

def synthesise(expected):
    """A component list, an SPDX and a CycloneDX document that name exactly what is expected."""
    items, packages, components = [], [], []
    for identifier, entry in sorted(expected.items()):
        hashes = [{"algorithm": a, "value": v} for a, v in sorted(entry["hashes"])]
        items.append({"id": identifier, "name": identifier, "version": entry["version"], "hashes": hashes})
        package = {"SPDXID": "SPDXRef-Package-native-" + identifier, "name": identifier,
                   "checksums": [{"algorithm": a.replace("-", ""), "checksumValue": v} for a, v in sorted(entry["hashes"])]}
        component = {"bom-ref": "hmailserver-native-" + identifier, "name": identifier,
                     "hashes": [{"alg": a, "content": v} for a, v in sorted(entry["hashes"])]}
        if entry["version"]:
            package["versionInfo"] = entry["version"]
            component["version"] = entry["version"]
        packages.append(package)
        components.append(component)
    return items, {"spdxVersion": "SPDX-2.3", "packages": packages}, {"bomFormat": "CycloneDX", "components": components}


def self_test(model):
    failures = []
    problems, expected = analyse(model)
    if problems:
        failures.append("the tree itself is not clean, so the controls prove nothing: " + problems[0])
        return failures
    items, spdx, cyclonedx = synthesise(expected)
    for name, found in (("a list naming everything", components_from_list(items)),
                        ("an SPDX document naming everything", components_from_document(spdx)),
                        ("a CycloneDX document naming everything", components_from_document(cyclonedx))):
        if check_components(expected, found, "it"):
            failures.append("%s was refused: %s" % (name, check_components(expected, found, "it")[0]))

    runtime = [a for a in model["manifest"]["artifacts"] if a.get("disposition") == "downloaded"][0]
    runtime_id = parse_producer(model["producer"])["installer"][runtime["component"]]
    fetched = [a for a in model["manifest"]["artifacts"] if a.get("disposition") == "fetched"][0]
    fetched_id = parse_producer(model["producer"])["installer"][fetched["component"]]

    def tree_case(label, change, needle):
        broken = copy.deepcopy(model)
        change(broken)
        found = analyse(broken)[0]
        if not any(needle in p for p in found):
            failures.append("%s was not refused (expected a problem naming %r; got %s)" % (label, needle, found or "nothing"))

    def list_case(label, change, needle):
        broken = copy.deepcopy(items)
        change(broken)
        found = check_components(expected, components_from_list(broken), "the list")
        if not any(needle in p for p in found):
            failures.append("%s was not refused (expected a problem naming %r; got %s)" % (label, needle, found or "nothing"))

    def document_case(label, document, change, needle):
        broken = copy.deepcopy(document)
        change(broken)
        found = check_components(expected, components_from_document(broken), "the document")
        if not any(needle in p for p in found):
            failures.append("%s was not refused (expected a problem naming %r; got %s)" % (label, needle, found or "nothing"))

    def add_artifact(m, kind, component):
        m["manifest"]["artifacts"].append({"path": "hmailserver/installation/Extras/example.dll", "disposition": kind, "component": component,
                                           "version": "1.0", "sha256": "0" * 64, "source": "build-inputs-1/7za-19.00.exe"})

    # The tree: an input pinned and not named.
    tree_case("a fetched component native-dependencies.ps1 has no entry for", lambda m: add_artifact(m, "fetched", "Example Library"), "Example Library")
    tree_case("a kind nobody has decided about", lambda m: add_artifact(m, "mirrored", "Example Library"), "'mirrored'")
    tree_case("a kind get-installer-binaries.ps1 places that nobody has decided about",
              lambda m: m.update(installer_script=m["installer_script"] + "\n$x = $a | Where-Object { $_.disposition -eq 'mirrored' }\n"), "'mirrored'")
    tree_case("native-dependencies.ps1 no longer emitting the downloaded entries",
              lambda m: m.update(producer=m["producer"].replace("@('fetched', 'gathered', 'downloaded')", "@('fetched', 'gathered')")), "'downloaded'")
    tree_case("get-dotnet-runtime.ps1 pinning an entry the manifest does not have",
              lambda m: m.update(runtime_script=m["runtime_script"].replace("windowsdesktop-runtime-10.0-win-x64.exe", "windowsdesktop-runtime-11.0-win-x64.exe")), "11.0")
    tree_case("the runtime pinned to a moving link",
              lambda m: [a.update(source="https://aka.ms/dotnet/10.0/windowsdesktop-runtime-win-x64.exe") for a in m["manifest"]["artifacts"] if a.get("disposition") == "downloaded"], "moving link")
    tree_case("the runtime with no SHA-512",
              lambda m: [a.update(sha512=None) for a in m["manifest"]["artifacts"] if a.get("disposition") == "downloaded"], "no SHA-512")
    tree_case("a fetched file whose build input has no hash",
              lambda m: m["manifest"].update(build_inputs=[]), "gives no SHA-256")
    tree_case("a library the Linux build finds and the list does not name",
              lambda m: m.update(cmake=m["cmake"] + "\nfind_package(Sodium REQUIRED)\n"), "Sodium")
    tree_case("a dependency lookup nobody reads",
              lambda m: m.update(cmake=m["cmake"] + "\npkg_check_modules(SODIUM REQUIRED libsodium)\n"), "pkg_check_modules")
    tree_case("a library directory the Windows build reaches and the list does not name",
              lambda m: m.update(vcxproj=m["vcxproj"] + "\n<AdditionalIncludeDirectories>$(hMailServerLibs)\\libsodium-1.0.20\\include</AdditionalIncludeDirectories>\n"), "libsodium-1.0.20")
    tree_case("a library directory another native project reaches and the list does not name",
              lambda m: m.update(other_projects=m["other_projects"] + "\n<AdditionalLibraryDirectories>$(hMailServerLibs)\\zstd-1.5.7\\lib</AdditionalLibraryDirectories>\n"), "zstd-1.5.7")
    tree_case("third-party source with no vendored entry",
              lambda m: m.update(licensed_dirs=m["licensed_dirs"] + ["libsodium"]), "libsodium")

    # The component list and the documents: named, at the pinned version, with the pinned hash.
    list_case("a list without the .NET runtime", lambda l: l.remove(next(i for i in l if i["id"] == runtime_id)), runtime_id)
    list_case("a list with another SHA-512 for the .NET runtime",
              lambda l: next(i for i in l if i["id"] == runtime_id)["hashes"][0].update(value="f" * 128), "SHA-512")
    list_case("a list without the hash of a fetched build input",
              lambda l: next(i for i in l if i["id"] == fetched_id).update(hashes=[]), "SHA-256")
    list_case("a list with OpenSSL at another version", lambda l: next(i for i in l if i["id"] == "openssl").update(version="3.0.0"), "openssl")
    list_case("a list that gives a distribution's library a version", lambda l: next(i for i in l if i["id"] == "linux-openssl").update(version="3.0.13"), "linux-openssl")
    document_case("an SPDX document without zlib", spdx,
                  lambda d: d.update(packages=[p for p in d["packages"] if p["SPDXID"] != "SPDXRef-Package-native-zlib"]), "zlib")
    document_case("a CycloneDX document with another hash for a fetched build input", cyclonedx,
                  lambda d: next(c for c in d["components"] if c["bom-ref"] == "hmailserver-native-" + fetched_id)["hashes"][0].update(content="0" * 64), fetched_id)
    return failures


# ------------------------------------------------------------------ main

def main(argv):
    model = load()
    if "--self-test" in argv:
        failures = self_test(model)
        for failure in failures:
            print("  FAIL  " + failure)
        if failures:
            return 1
        print("check-sbom-inputs self-test: every pinned input left out, every wrong version and every wrong hash is refused")
        return 0

    documents = [argv[i + 1] for i, arg in enumerate(argv) if arg == "--sbom" and i + 1 < len(argv)]
    problems, expected = analyse(model)
    checked = []

    if documents:
        for path in documents:
            try:
                found = components_from_document(json.loads(read(path)))
            except (OSError, ValueError) as error:
                problems.append("%s cannot be read as an SBOM: %s" % (path, error))
                continue
            problems.extend(check_components(expected, found, os.path.basename(path)))
            checked.append(os.path.basename(path))
    else:
        items, shell = run_producer()
        if items is None and shell:
            problems.append(shell)
        elif items is not None:
            problems.extend(check_components(expected, components_from_list(items), "native-dependencies.ps1's output"))
            checked.append("native-dependencies.ps1's output (run with %s)" % shell)

    for problem in problems:
        print("  FAIL  " + problem)
    if problems:
        print("%d problem(s). A pinned input the SBOM does not name is a bill of materials that is silently one short." % len(problems))
        return 1

    kinds = {}
    for identifier in expected:
        kind = "Linux" if identifier.startswith("linux-") else "other"
        kinds[kind] = kinds.get(kind, 0) + 1
    print("check-sbom-inputs: %d components the SBOM must name beyond Syft's (%d for the Windows build and installer, %d for the Linux packages), "
          "every pinned input among them" % (len(expected), kinds.get("other", 0), kinds.get("Linux", 0)))
    if checked:
        print("  checked against " + ", ".join(checked))
    else:
        print("  (no PowerShell on this machine: native-dependencies.ps1's tables were checked, not run)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
