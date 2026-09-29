#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The aur-publish job of .gitlab/ci/channels.yml: the release on the Arch User
Repository as the package hmailserver (hmailserver/docs/Channels.md, "The AUR").

  1. Dormant: without the project's CI/CD variable AUR_SSH_KEY (the private half of
     the maintainers' ed25519 key, base64, protected and masked, whose public half is
     on the AUR account) it says "not configured" and exits 0.
  2. The release, as every channel takes it (build/ci/channels/release-files.py):
     HM_CHANNEL_VERSION or the feed's latest, which the feed must list as published.
  3. The release's source tarball, hmailserver-<v>.tar.gz, downloaded and verified
     against its Sigstore bundle as this project's pipeline for the tag, before
     anything is sent.
  4. The PKGBUILD rendered from hmailserver/source/Server/platform/packaging/PKGBUILD
     as master has it: pkgver the release's, sha256sums the verified tarball's
     SHA-256, pkgrel 1 for a release the AUR does not have, or the AUR's pkgrel plus
     one when the same release's PKGBUILD has changed on master since it was pushed;
     and .SRCINFO from it by makepkg --printsrcinfo, run as an unprivileged user.
  5. Pushed to ssh://aur@aur.archlinux.org/hmailserver.git over SSH with the key,
     StrictHostKeyChecking on, and the AUR's host keys from
     build/ci/channels/aur.known_hosts alone, each held to the fingerprint the AUR
     publishes. Nothing is pushed when the AUR has the rendered files already, and a
     release older than the AUR's is refused.

The key is written to a file of the job's own, mode 0600, and never printed or put
on a command line; the log names its public fingerprint.

Usage, from the root of a checkout, in an Arch Linux container:
  python3 build/ci/channels/aur-publish.py [--out DIR]
  python3 build/ci/channels/aur-publish.py render --version V --sha256 HEX [--pkgrel N] [--out DIR]
        the PKGBUILD and .SRCINFO for a version and a tarball's SHA-256, offline
  python3 build/ci/channels/aur-publish.py --self-test
"""

import argparse
import base64
import hashlib
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def load(name, path):
    """A module of this repository by its path, once per process: every channel script
    shares one release-files.py, and so one Refused, whichever loads it first."""
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


rf = load("release_files", os.path.join(HERE, "release-files.py"))
Refused = rf.Refused

PKGBUILD = os.path.join("hmailserver", "source", "Server", "platform", "packaging", "PKGBUILD")
KNOWN_HOSTS = os.path.join(HERE, "aur.known_hosts")
AUR_REMOTE = "ssh://aur@aur.archlinux.org/hmailserver.git"
AUR_HOST = "aur.archlinux.org"
# The fingerprints https://aur.archlinux.org/ lists on its home page (28 September 2026).
AUR_FINGERPRINTS = {
    "ssh-ed25519": "SHA256:RFzBCUItH9LZS0cKB5UE6ceAYhBD5C8GeOBip8Z11+4",
    "ecdsa-sha2-nistp256": "SHA256:uTa/0PndEgPZTf76e1DFqXKJEXKsn7m9ivhLQtzGOCI",
    "ssh-rsa": "SHA256:5s5cIyReIfNNVGRFdDbe3hdYiI5OelHGpw2rOUud3Q8",
}
SOURCE_LINE = ('source=("https://gitlab.com/api/v4/projects/86730559/packages/generic/hmailserver/${pkgver}/'
               'hmailserver-${pkgver}.tar.gz")')
COMMITTER = ("Christopher Holloway", "chris.holloway@progressiverobot.com")
BUILD_USER = "builder"


def say(line=""):
    print(line)
    sys.stdout.flush()


# ---------------------------------------------------------------- the PKGBUILD

def render(template, version, sha256, pkgrel=1):
    """The PKGBUILD for a release: the template's pkgver, pkgrel and sha256sums lines
    replaced, and nothing else. Refused when the template no longer has the shape this
    fills in."""
    if not re.match(r"^\d+\.\d+\.\d+$", version):
        raise Refused("%r is not a release version" % version)
    if not re.match(r"^[0-9a-f]{64}$", sha256):
        raise Refused("%r is not a SHA-256" % sha256)
    text = template.replace("\r\n", "\n")
    for pattern, line in ((r"^pkgver=.*$", "pkgver=%s" % version), (r"^pkgrel=.*$", "pkgrel=%d" % pkgrel),
                          (r"^sha256sums=.*$", "sha256sums=('%s')" % sha256)):
        text, count = re.subn(pattern, line, text, count=0, flags=re.MULTILINE)
        if count != 1:
            raise Refused("%s has %d line(s) matching %s, not one: the template has changed shape" % (PKGBUILD, count, pattern))
    if SOURCE_LINE not in text.split("\n"):
        raise Refused("%s's source= is not the release's tarball in the package registry (%s)" % (PKGBUILD, SOURCE_LINE))
    return text


def fields(pkgbuild):
    """(pkgver, pkgrel) of a PKGBUILD's text, or (None, None)."""
    version = re.search(r"^pkgver=(\S+)\s*$", pkgbuild or "", re.MULTILINE)
    release = re.search(r"^pkgrel=(\d+)\s*$", pkgbuild or "", re.MULTILINE)
    return (version.group(1) if version else None, int(release.group(1)) if release else None)


def version_key(version):
    return tuple(int(p) for p in version.split("."))


def printsrcinfo(pkgbuild_text, run=subprocess.run):
    """makepkg --printsrcinfo of a PKGBUILD, run as an unprivileged user when this runs
    as root (makepkg will not run as root)."""
    work = tempfile.mkdtemp(prefix="hm-aur-srcinfo-")
    try:
        with open(os.path.join(work, "PKGBUILD"), "w", encoding="utf-8", newline="\n") as f:
            f.write(pkgbuild_text)
        command = ["makepkg", "--printsrcinfo"]
        if hasattr(os, "geteuid") and os.geteuid() == 0:
            run(["chown", "-R", BUILD_USER, work], check=True)
            # The build user's own directory, holding only the PKGBUILD makepkg reads. (Semgrep
            # takes whatever follows "nosemgrep:" as rule ids, so the reason is here.)
            os.chmod(work, 0o755)  # nosemgrep
            command = ["runuser", "-u", BUILD_USER, "--"] + command
        answer = run(command, cwd=work, capture_output=True, text=True)
        if answer.returncode != 0:
            raise Refused("makepkg --printsrcinfo failed: %s" % (answer.stderr or answer.stdout).strip()[-600:])
        return answer.stdout
    finally:
        shutil.rmtree(work, ignore_errors=True)


def check_srcinfo(srcinfo, version, pkgrel, sha256):
    """What the AUR shows is what was rendered: problems as sentences."""
    problems = []
    wanted = {"pkgbase": "hmailserver", "pkgver": version, "pkgrel": str(pkgrel), "sha256sums": sha256,
              "source": "https://gitlab.com/api/v4/projects/86730559/packages/generic/hmailserver/%s/hmailserver-%s.tar.gz"
                        % (version, version)}
    found = {}
    for line in srcinfo.splitlines():
        match = re.match(r"^\s*(\w+) = (.*)$", line)
        if match and match.group(1) in wanted:
            found.setdefault(match.group(1), []).append(match.group(2))
    for key, value in wanted.items():
        if found.get(key) != [value]:
            problems.append(".SRCINFO's %s is %s, not %s" % (key, found.get(key), value))
    return problems


# ---------------------------------------------------------------- the AUR's host keys

def ssh_fingerprint(blob64):
    return "SHA256:" + base64.b64encode(hashlib.sha256(base64.b64decode(blob64)).digest()).decode().rstrip("=")


def known_hosts_lines(path=KNOWN_HOSTS):
    """The pinned lines, each held to the AUR's published fingerprint; Refused if any
    is not, or one of the three is missing."""
    lines, seen = [], set()
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 3 or parts[0] != AUR_HOST or parts[1] not in AUR_FINGERPRINTS:
                raise Refused("%s has a line that is not one of the AUR's three host keys: %s" % (path, line[:60]))
            if ssh_fingerprint(parts[2]) != AUR_FINGERPRINTS[parts[1]]:
                raise Refused("%s's %s key is %s, not %s, the fingerprint the AUR publishes"
                              % (path, parts[1], ssh_fingerprint(parts[2]), AUR_FINGERPRINTS[parts[1]]))
            seen.add(parts[1])
            lines.append(" ".join(parts[:3]))
    if seen != set(AUR_FINGERPRINTS):
        raise Refused("%s lacks the AUR's %s key(s)" % (path, ", ".join(sorted(set(AUR_FINGERPRINTS) - seen))))
    return lines


# ---------------------------------------------------------------- the job

def aur_publish(out, environ=None, run=subprocess.run, root=ROOT):
    environ = os.environ if environ is None else environ
    fix = rf.fixture(environ)
    if not environ.get("AUR_SSH_KEY"):
        say("not configured: AUR_SSH_KEY is not set, so nothing was published.")
        say("The owner signs in to the AUR; a maintainer adds the public half of a new ed25519 key to that")
        say("account and stores the private half, base64, as the protected, masked CI/CD variable AUR_SSH_KEY")
        say("(hmailserver/docs/Channels.md, 'The AUR').")
        return 0

    version, where = rf.release_version(environ, fix)
    say("the release: %s (from %s)" % (version, where))
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    tarball = "hmailserver-%s.tar.gz" % version
    cosign = rf.tool_path("cosign", out, environ)
    record = rf.verified_files(version, [tarball], os.path.join(out, "release"), cosign, fix)
    rf.write_record(record, version, where, os.path.join(out, "verification.json"))
    sha256 = record[tarball]["sha256"]
    with open(os.path.join(root, PKGBUILD), encoding="utf-8") as f:
        template = f.read()

    # The key and the host keys, in files of the job's own, and git's ssh told to
    # use those and nothing else.
    secrets = tempfile.mkdtemp(prefix="hm-aur-ssh-")
    try:
        key_path = os.path.join(secrets, "id_aur")
        try:
            key = base64.b64decode(environ["AUR_SSH_KEY"].strip(), validate=True)
        except ValueError:
            raise Refused("AUR_SSH_KEY is not base64 (base64 -w0 of the private key file)")
        descriptor = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "wb") as f:
            f.write(key if key.endswith(b"\n") else key + b"\n")
        key = None
        public = run(["ssh-keygen", "-y", "-f", key_path], capture_output=True, text=True)
        if public.returncode != 0:
            raise Refused("AUR_SSH_KEY does not hold a private key ssh-keygen can read (no passphrase)")
        blob = public.stdout.split()[1] if len(public.stdout.split()) > 1 else ""
        say("pushing as the key %s %s" % (public.stdout.split()[0] if public.stdout.split() else "?", ssh_fingerprint(blob)))
        hosts_path = os.path.join(secrets, "known_hosts")
        with open(hosts_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(known_hosts_lines()) + "\n")
        git_env = dict(environ)
        git_env["GIT_SSH_COMMAND"] = ("ssh -i %s -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=%s"
                                      " -o GlobalKnownHostsFile=/dev/null -o BatchMode=yes" % (key_path, hosts_path))
        git_env["GIT_TERMINAL_PROMPT"] = "0"
        remote = fix.get("aur_remote") if fix else AUR_REMOTE

        clone = os.path.join(out, "aur")

        def git(*arguments, **kwargs):
            answer = run(["git", "-C", clone] + list(arguments), capture_output=True, text=True, env=git_env, **kwargs)
            if answer.returncode != 0:
                raise Refused("git %s failed: %s" % (arguments[0], (answer.stderr or answer.stdout).strip()[-400:]))
            return answer.stdout

        cloned = run(["git", "clone", "--quiet", remote, clone], capture_output=True, text=True, env=git_env)
        if cloned.returncode != 0:
            raise Refused("the AUR's repository %s could not be cloned: %s" % (remote, (cloned.stderr or "").strip()[-400:]))
        held_text = None
        if os.path.isfile(os.path.join(clone, "PKGBUILD")):
            with open(os.path.join(clone, "PKGBUILD"), encoding="utf-8") as f:
                held_text = f.read()
        held_version, held_release = fields(held_text)
        pkgrel = 1
        if held_version:
            say("the AUR has hmailserver %s-%s" % (held_version, held_release))
            if version_key(held_version) > version_key(version):
                raise Refused("the AUR has %s, newer than the release %s: a channel never goes back. Nothing was pushed."
                              % (held_version, version))
            if held_version == version:
                if render(template, version, sha256, held_release or 1) == held_text:
                    say("The AUR has this release's PKGBUILD already, byte for byte. Nothing to push.")
                    return 0
                pkgrel = (held_release or 0) + 1
                say("master's PKGBUILD has changed since %s-%s was pushed: pushing %s-%d"
                    % (version, held_release, version, pkgrel))
        text = render(template, version, sha256, pkgrel)
        srcinfo = printsrcinfo(text, run)
        problems = check_srcinfo(srcinfo, version, pkgrel, sha256)
        if problems:
            raise Refused(".SRCINFO is not what was rendered:\n  " + "\n  ".join(problems))
        for name, data in (("PKGBUILD", text), (".SRCINFO", srcinfo)):
            for where_to in (clone, out):
                with open(os.path.join(where_to, name), "w", encoding="utf-8", newline="\n") as f:
                    f.write(data)
        git("add", "PKGBUILD", ".SRCINFO")
        git("-c", "user.name=" + COMMITTER[0], "-c", "user.email=" + COMMITTER[1], "commit", "--quiet", "-m",
            "hmailserver %s-%d\n\nThe release's source tarball, hmailserver-%s.tar.gz (SHA-256 %s), verified against its"
            " Sigstore bundle as %s.\n" % (version, pkgrel, version, sha256, record[tarball]["identity"]))
        head = git("rev-parse", "HEAD").strip()
        git("push", "--quiet", "origin", "HEAD:master")
        listed = run(["git", "-C", clone, "ls-remote", "origin", "refs/heads/master"], capture_output=True, text=True,
                     env=git_env)
        if listed.returncode != 0 or not listed.stdout.startswith(head):
            raise Refused("the AUR's master is not %s after the push: %s" % (head, (listed.stdout or listed.stderr).strip()))
        say("PUBLISHED: hmailserver %s-%d on the AUR (%s), commit %s." % (version, pkgrel, remote, head))
        return 0
    finally:
        shutil.rmtree(secrets, ignore_errors=True)


# ---------------------------------------------------------------- self-test

class Answer:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def self_test():
    failures = []

    def expect(what, condition):
        print("%s  %s" % ("ok  " if condition else "FAIL", what))
        if not condition:
            failures.append(what)

    def refused(what, call, needle):
        try:
            call()
        except Refused as error:
            expect("%s (%s)" % (what, str(error).splitlines()[0][:110]), needle in str(error))
            return
        expect(what + ": not refused", False)

    with open(os.path.join(ROOT, PKGBUILD), encoding="utf-8") as f:
        template = f.read()
    sha = "ab" * 32
    text = render(template, "9.9.9", sha, 3)
    expect("the render sets pkgver, pkgrel and sha256sums, and nothing else", fields(text) == ("9.9.9", 3)
           and "sha256sums=('%s')" % sha in text
           and [l for l in template.replace("\r\n", "\n").split("\n") if not re.match(r"^(pkgver|pkgrel|sha256sums)=", l)]
           == [l for l in text.split("\n") if not re.match(r"^(pkgver|pkgrel|sha256sums)=", l)])
    expect("the tree's PKGBUILD builds the release's tarball from the package registry", SOURCE_LINE in text.split("\n"))
    expect("  ... as a distribution's build, without the live update", "-DHM_LIVE_UPDATE=OFF" in text)
    refused("a template whose source is not the release's tarball",
            lambda: render(template.replace(SOURCE_LINE, 'source=("git+https://example.org/x.git")'), "9.9.9", sha), "source=")
    refused("a template with two pkgver lines", lambda: render(template + "\npkgver=1\n", "9.9.9", sha), "not one")
    refused("a pre-release", lambda: render(template, "9.9.9-rc1", sha), "not a release version")

    srcinfo = ("pkgbase = hmailserver\n\tpkgver = 9.9.9\n\tpkgrel = 3\n\tsource = https://gitlab.com/api/v4/projects/86730559/"
               "packages/generic/hmailserver/9.9.9/hmailserver-9.9.9.tar.gz\n\tsha256sums = %s\n\npkgname = hmailserver\n" % sha)
    expect(".SRCINFO held to what was rendered", check_srcinfo(srcinfo, "9.9.9", 3, sha) == [])
    expect("  ... and a .SRCINFO that differs caught", check_srcinfo(srcinfo.replace("pkgrel = 3", "pkgrel = 2"), "9.9.9", 3, sha))

    expect("the pinned AUR host keys are the three the AUR publishes", len(known_hosts_lines()) == 3)
    work = tempfile.mkdtemp(prefix="hm-aur-test-")
    try:
        forged = os.path.join(work, "known_hosts")
        with open(KNOWN_HOSTS, encoding="utf-8") as f:
            pinned = f.read()
        with open(forged, "w", encoding="utf-8") as f:
            f.write(pinned.replace("AAAAC3NzaC1lZDI1NTE5AAAAIEuBKrPzbawxA", "AAAAC3NzaC1lZDI1NTE5AAAAIEuBKrPzbawxB"))
        refused("a host key that is not the AUR's", lambda: known_hosts_lines(forged), "not SHA256:RFzBCU")
        with open(forged, "w", encoding="utf-8") as f:
            f.write("\n".join(l for l in pinned.splitlines() if "ssh-rsa" not in l))
        refused("a host key missing", lambda: known_hosts_lines(forged), "lacks the AUR's ssh-rsa")

        # The job, end to end: dormant without the key, and with a fixture key it
        # never reaches git when the tarball does not verify.
        said = []
        held_say = globals()["say"]
        globals()["say"] = said.append
        try:
            expect("without AUR_SSH_KEY: 'not configured', exit 0, nothing read",
                   aur_publish(os.path.join(work, "a"), {}, run=None) == 0 and said[0].startswith("not configured"))
        finally:
            globals()["say"] = held_say
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            return Answer(1)
        held = (rf.release_version, rf.tool_path, rf.verified_files)
        try:
            rf.release_version = lambda environ, fix: ("9.9.9", "HM_CHANNEL_VERSION")
            rf.tool_path = lambda tool, work, environ=None: "cosign"

            def unverified(*args, **kwargs):
                raise Refused("hmailserver-9.9.9.tar.gz does not verify as signed by this project's pipeline for v9.9.9")
            rf.verified_files = unverified
            refused("with a key, a tarball that does not verify: refused, and no git, ssh or makepkg ran",
                    lambda: aur_publish(os.path.join(work, "b"), {"AUR_SSH_KEY": base64.b64encode(b"k").decode()}, run=run),
                    "does not verify")
            expect("  ... nothing was run", calls == [])
        finally:
            rf.release_version, rf.tool_path, rf.verified_files = held
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("")
    if failures:
        print("%d self-test case(s) failed" % len(failures))
        return 1
    print("Every self-test case passed.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--out", default="aur-publish")
    commands = parser.add_subparsers(dest="command")
    r = commands.add_parser("render", help="the PKGBUILD and .SRCINFO for a version and a SHA-256, offline")
    r.add_argument("--version", required=True)
    r.add_argument("--sha256", required=True)
    r.add_argument("--pkgrel", type=int, default=1)
    r.add_argument("--out", default="aur-render")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    try:
        if arguments.command == "render":
            with open(os.path.join(ROOT, PKGBUILD), encoding="utf-8") as f:
                text = render(f.read(), arguments.version, arguments.sha256, arguments.pkgrel)
            os.makedirs(arguments.out, exist_ok=True)
            with open(os.path.join(arguments.out, "PKGBUILD"), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            srcinfo = printsrcinfo(text)
            problems = check_srcinfo(srcinfo, arguments.version, arguments.pkgrel, arguments.sha256)
            if problems:
                raise Refused(".SRCINFO is not what was rendered:\n  " + "\n  ".join(problems))
            with open(os.path.join(arguments.out, ".SRCINFO"), "w", encoding="utf-8", newline="\n") as f:
                f.write(srcinfo)
            say("rendered: %s/PKGBUILD and %s/.SRCINFO" % (arguments.out, arguments.out))
            return 0
        return aur_publish(os.path.abspath(arguments.out))
    except Refused as error:
        say("REFUSED: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
