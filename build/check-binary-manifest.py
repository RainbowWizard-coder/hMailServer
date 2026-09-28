#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every binary committed to the repository is in its manifest, with the SHA-256 it has.

hmailserver/docs/third-party-binaries.json is the record of every binary file the
build uses (hmailserver/docs/ThirdPartyBinaries.md is its policy): what it is,
where it comes from, why the build needs it and, for a file that is committed, its
SHA-256. Until 18 September 2026 a GitHub workflow held the tree to it
(verify-binary-provenance.yml, in the history at v6.3.3); nothing had since. This
is that check, in Python, run by the checkers job of .gitlab-ci.yml in every
pipeline - a merge request's included, on GitLab's shared runners, with no
secret - since that job runs every build/check-*.py. It reads the files git
tracks (git ls-files), and fails when:

  UNLISTED  a tracked file is a binary the manifest does not list. A binary is a
            file whose extension is one of OpenSSF Scorecard's Binary-Artifacts
            set (checks/raw/binary_artifact.go, as the old workflow copied it), or
            whose first bytes are a PE image's ("MZ"), an ELF object's, an OLE
            compound file's (an .msi), a Mach-O image's, a Java class's, a
            WebAssembly module's, an ar archive's (a static library, a .deb), an
            RPM package's, an Android DEX file's or a Chrome extension's (the
            last three Scorecard's own check reads by their first bytes too): an
            extension-only scan missed the two ADO type libraries, PE images
            named .tlb, when they were still committed. "MZ"
            counts only with a NUL byte in the first 64, as every DOS and PE
            header has and no text file does, so a text file that begins with
            those two letters is not taken for an executable;
  TRACKED   a file the manifest marks as not in git (fetched, gathered, system,
            generated, downloaded: the build fetches, gathers or makes it) is
            tracked after all;
  MISSING   a file the manifest marks as committed (retain, retain-generated) is
            not tracked: remove its entry in the same change that removes it;
  CHANGED   a committed file's bytes are not the SHA-256 its entry records: a
            changed binary is updated in its entry - sha256, version, upstream -
            in the same change, which is the review;
  and when the manifest itself is not what this reads: not JSON, an entry
  without a path or with a disposition the manifest's own "dispositions" table
  does not define, a path listed twice, or a committed entry without a SHA-256.

What the tree holds that is binary only in the plain sense - the icon library's
images, the documents' pictures, the OpenPGP fixtures - has none of those
extensions or headers, and is not in the manifest (ThirdPartyBinaries.md says why).

Usage:
  python build/check-binary-manifest.py              the tree, from any directory
  python build/check-binary-manifest.py --self-test  each failure planted in a
                                                     repository of its own, and caught
Exits 0 when every committed binary is accounted for, 1 when not, 2 when git
cannot list the tree.
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = "hmailserver/docs/third-party-binaries.json"
POLICY = "hmailserver/docs/ThirdPartyBinaries.md"

# Scorecard's Binary-Artifacts extensions, as the old workflow copied them from
# checks/raw/binary_artifact.go.
BINARY_EXTENSIONS = {
    "crx", "deb", "dex", "dey", "elf", "o", "a", "so", "macho", "iso", "class",
    "jar", "bundle", "dylib", "lib", "msi", "dll", "drv", "efi", "exe", "ocx",
    "pyc", "pyo", "par", "rpm", "wasm", "whl",
}
# The first bytes of an executable or an installer, whatever the file is called.
MAGIC = [
    (b"MZ", "a PE (Windows) image"),
    (b"\x7fELF", "an ELF object"),
    (b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", "an OLE compound file (an .msi)"),
    (b"\xfe\xed\xfa\xce", "a Mach-O image"),
    (b"\xfe\xed\xfa\xcf", "a Mach-O image"),
    (b"\xce\xfa\xed\xfe", "a Mach-O image"),
    (b"\xcf\xfa\xed\xfe", "a Mach-O image"),
    (b"\xca\xfe\xba\xbe", "a Java class or a universal Mach-O image"),
    (b"\x00asm", "a WebAssembly module"),
    # What Scorecard's own check also knows by its first bytes (the filetype library
    # it reads a file's head with), and the archive every static library is: a .deb
    # and a .a or MinGW .lib begin "!<arch>", an .rpm with its lead, an Android .dex
    # or .dey with "dex"/"dey" and its version, a Chrome extension with "Cr24".
    (b"!<arch>\n", "an ar archive (a static library or a .deb)"),
    (b"\xed\xab\xee\xdb", "an RPM package"),
    (b"dex\n0", "an Android DEX file"),
    (b"dey\n0", "an Android DEY file"),
    (b"Cr24", "a Chrome extension (.crx)"),
]
# The dispositions of a file git does not hold, and of one it does.
NOT_IN_GIT = {"fetched", "gathered", "system", "generated", "downloaded"}
IN_GIT = {"retain", "retain-generated"}
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def tracked_files(root):
    """The paths git tracks under root, with forward slashes; None when git cannot say."""
    try:
        listed = subprocess.run(["git", "-C", root, "ls-files", "-z"], capture_output=True, check=False)
    except OSError:
        return None
    if listed.returncode != 0:
        return None
    return [p.decode("utf-8", "surrogateescape") for p in listed.stdout.split(b"\0") if p]


def kind_of(root, path):
    """Why a file is a binary, or None."""
    name = path.rsplit("/", 1)[-1]
    if "." in name:
        extension = name.rsplit(".", 1)[1].lower()
        if extension in BINARY_EXTENSIONS:
            return "its extension is .%s" % extension
    full = os.path.join(root, *path.split("/"))
    try:
        with open(full, "rb") as f:
            head = f.read(64)
    except OSError:
        # A submodule, a symbolic link to a directory, a path the checkout left out:
        # nothing to read, and nothing git carries as a file's bytes here.
        return None
    for magic, what in MAGIC:
        if head.startswith(magic):
            if magic == b"MZ" and b"\x00" not in head:
                continue
            return "it begins as %s does" % what
    return None


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def check(root):
    """(problems, notes) for the tree at root."""
    problems, notes = [], []
    tracked = tracked_files(root)
    if tracked is None:
        return None, ["git cannot list the files of %s: run this in a git checkout" % root]
    path = os.path.join(root, *MANIFEST.split("/"))
    try:
        with open(path, encoding="utf-8") as f:
            manifest = json.load(f)
    except OSError:
        return ["%s is missing: it is where every binary the build uses is recorded (%s)" % (MANIFEST, POLICY)], notes
    except ValueError as e:
        return ["%s is not JSON: %s" % (MANIFEST, e)], notes
    artifacts = manifest.get("artifacts") if isinstance(manifest, dict) else None
    if not isinstance(artifacts, list):
        return ["%s has no list of artifacts" % MANIFEST], notes
    defined = set((manifest.get("dispositions") or {}).keys()) - {"platform"}

    in_git = set(tracked)
    listed = {}
    for number, entry in enumerate(artifacts, 1):
        where = "%s, artifact %d" % (MANIFEST, number)
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str) or not entry["path"].strip():
            problems.append("%s has no path" % where)
            continue
        name = entry["path"]
        where = "%s (%s)" % (where, name)
        if name in listed:
            problems.append("%s is listed twice (artifacts %d and %d)" % (name, listed[name], number))
            continue
        listed[name] = number
        disposition = entry.get("disposition")
        if disposition not in defined or disposition not in NOT_IN_GIT | IN_GIT:
            problems.append("%s: its disposition %r is not one the manifest defines and this check knows (%s)" % (
                where, disposition, ", ".join(sorted(NOT_IN_GIT | IN_GIT))))
            continue
        if disposition in NOT_IN_GIT:
            if name in in_git:
                problems.append("TRACKED   %s is marked '%s' but is committed: remove it from git, since the build "
                                "fetches, gathers or makes it (%s)" % (name, disposition, POLICY))
            continue
        expected = (entry.get("sha256") or "").lower()
        if not SHA256.match(expected):
            problems.append("%s: a committed file's entry gives its SHA-256 (64 hex digits), and this one gives %r" % (
                where, entry.get("sha256")))
            continue
        if name not in in_git:
            problems.append("MISSING   %s is marked '%s' (committed) and git does not track it: if it was removed on "
                            "purpose, remove its entry in the same change" % (name, disposition))
            continue
        full = os.path.join(root, *name.split("/"))
        try:
            actual = sha256_of(full)
        except OSError as e:
            problems.append("MISSING   %s is tracked but cannot be read here: %s" % (name, e))
            continue
        if actual != expected:
            problems.append("CHANGED   %s: the manifest records SHA-256 %s and the file is %s. A committed binary "
                            "changed: update its entry - sha256, version, upstream - in the same change, knowing why "
                            "(%s, 'Updating the manifest')" % (name, expected, actual, POLICY))

    binaries = 0
    for name in tracked:
        why = kind_of(root, name)
        if why is None:
            continue
        binaries += 1
        if name not in listed:
            problems.append("UNLISTED  %s is a binary (%s) that %s does not list. Do not commit it: the build fetches "
                            "what it needs (%s). If it must be in the tree, give it an entry - its SHA-256, upstream, "
                            "version, licence, and why - with the disposition 'retain'" % (name, why, MANIFEST, POLICY))
    notes.append("%d tracked files read, %d of them binaries, %d manifest entries (%d committed)" % (
        len(tracked), binaries, len(listed),
        sum(1 for e in artifacts if isinstance(e, dict) and e.get("disposition") in IN_GIT)))
    return problems, notes


# ---------------------------------------------------------------- the self-test

PE = b"MZ\x90\x00" + b"\x00" * 60 + b"PE\x00\x00" + b"\x00" * 64
ELF = b"\x7fELF\x02\x01\x01" + b"\x00" * 57


def self_test():
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    work = tempfile.mkdtemp(prefix="binary-manifest-")
    try:
        def repository(files, manifest):
            root = os.path.join(work, "r%d" % len(os.listdir(work)))
            os.makedirs(root)
            # The files go into the index as they are: no line-ending conversion from
            # the caller's own git configuration, and nothing said about it.
            subprocess.run(["git", "init", "-q", root], check=True)
            subprocess.run(["git", "-C", root, "config", "core.autocrlf", "false"], check=True)
            subprocess.run(["git", "-C", root, "config", "core.safecrlf", "false"], check=True)
            content = dict(files)
            if manifest is not None:
                content[MANIFEST] = json.dumps(manifest, indent=1).encode("utf-8") if not isinstance(manifest, bytes) else manifest
            for name, data in content.items():
                full = os.path.join(root, *name.split("/"))
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "wb") as f:
                    f.write(data)
            subprocess.run(["git", "-C", root, "add", "-A"], check=True)
            return root

        dispositions = {"retain": "committed", "fetched": "not in git", "gathered": "not in git",
                        "system": "not in git", "generated": "not in git", "downloaded": "not in git",
                        "retain-generated": "committed", "platform": "not a disposition"}

        def manifest(*artifacts):
            return {"schema": "hmailserver-third-party-binaries/1", "dispositions": dispositions, "artifacts": list(artifacts)}

        fetched = {"path": "installation/Extras/7za.exe", "sha256": "0" * 64, "disposition": "fetched"}
        text = {"README.md": b"# a tree\n", "src/a.cpp": b"int main() { return 0; }\n",
                "docs/picture.png": b"\x89PNG\r\n\x1a\n" + b"\x00" * 16,
                "docs/letters.txt": b"MZ is only two letters here\n"}

        def run(files, m):
            problems, _ = check(repository(files, m))
            return problems or []

        found = run(text, manifest(fetched))
        expect(found == [], "a tree with no binary, whose manifest lists a fetched file, passes: %s" % found)
        clean_text = text

        def refused(what, files, m, needle):
            found = run(files, m)
            if not any(needle in p for p in found):
                failures.append("%s is refused (%s): %s" % (what, needle, found))

        refused("an unlisted DLL", dict(clean_text, **{"lib/tool.dll": b"not even a PE"}), manifest(fetched),
                "UNLISTED  lib/tool.dll")
        refused("a PE image named .tlb", dict(clean_text, **{"libraries/msado28/ado.tlb": PE}), manifest(fetched),
                "UNLISTED  libraries/msado28/ado.tlb")
        refused("an ELF object with no extension", dict(clean_text, **{"tools/helper": ELF}), manifest(fetched),
                "UNLISTED  tools/helper")
        refused("an .msi by its OLE header, named .bin", dict(clean_text, **{"setup/runtime.bin": MAGIC[2][0] + b"\x00" * 8}),
                manifest(fetched), "UNLISTED  setup/runtime.bin")
        refused("a fetched file committed after all", dict(clean_text, **{"installation/Extras/7za.exe": PE}), manifest(fetched),
                "TRACKED   installation/Extras/7za.exe")
        retained = {"path": "installation/tool.exe", "sha256": hashlib.sha256(PE).hexdigest(), "disposition": "retain"}
        found = run(dict(clean_text, **{"installation/tool.exe": PE}), manifest(fetched, retained))
        expect(found == [], "a committed binary listed with its SHA-256 passes: %s" % found)
        refused("a committed binary whose bytes changed", dict(clean_text, **{"installation/tool.exe": PE + b"\x01"}),
                manifest(fetched, retained), "CHANGED   installation/tool.exe")
        refused("a committed entry whose file is gone", clean_text, manifest(fetched, retained), "MISSING   installation/tool.exe")
        refused("a committed entry without a SHA-256", dict(clean_text, **{"installation/tool.exe": PE}),
                manifest(fetched, dict(retained, sha256=None)), "gives its SHA-256")
        refused("an entry of a disposition nobody defined", clean_text,
                manifest(fetched, {"path": "x.dll", "sha256": None, "disposition": "vendored"}), "is not one the manifest defines")
        refused("a path listed twice", clean_text, manifest(fetched, fetched), "is listed twice")
        refused("a manifest that is not JSON", clean_text, b"{ not json", "is not JSON")
        refused("no manifest at all", clean_text, None, "is missing")
        refused("a DOS image: MZ and a NUL, no PE header", dict(clean_text, **{"tools/old.com": b"MZ\x00\x01" + b"\x00" * 28}),
                manifest(fetched), "UNLISTED  tools/old.com")
        # Every other header, each in a file whose name says nothing.
        for number, (magic, what) in enumerate(MAGIC):
            if magic == b"MZ":
                continue
            name = "blobs/header-%d.dat" % number
            refused("%s named .dat" % what, dict(clean_text, **{name: magic + b"\x00" * 32}), manifest(fetched),
                    "UNLISTED  " + name)
        # The planted defect of Roadmap3's test: a merge request that adds an unlisted binary.
        refused("a binary added beside a clean manifest", dict(clean_text, **{"build/new-helper.exe": PE}), manifest(fetched),
                "UNLISTED  build/new-helper.exe")
        problems, notes = check(os.path.join(work, "not-a-repository"))
        expect(problems is None and notes and "git cannot list" in notes[0], "a directory git does not know is not a pass")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main(argv):
    if "--self-test" in argv[1:]:
        return self_test()
    if argv[1:]:
        print(__doc__.split("\n", 1)[0])
        print("Usage: python build/check-binary-manifest.py [--self-test]")
        return 2
    problems, notes = check(ROOT)
    if problems is None:
        for note in notes:
            print("FAIL  " + note)
        return 2
    for note in notes:
        print("      " + note)
    if problems:
        for problem in problems:
            print("FAIL  " + problem)
        print("%d problem(s) with the committed binaries; %s says what each means and how to fix it." % (len(problems), POLICY))
        return 1
    print("OK    every committed binary is in %s, at the SHA-256 it records" % MANIFEST)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
