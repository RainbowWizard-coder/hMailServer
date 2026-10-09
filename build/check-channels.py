#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The release channels' files held to what hmailserver/docs/Channels.md says of them.

  .gitlab/ci/channels.yml  every job but channels-check is created for a protected
                           master's own pipeline alone - never a tag's, a merge
                           request's, a schedule's or a downstream one - and
                           is started by hand and allowed to fail (chocolatey-publish
                           follows chocolatey-release instead); no job asks for
                           id_tokens or secrets; each runs a script of
                           build/ci/channels/ that is there; every job in a docker
                           image takes .no-github first; HM_REGCTL_IMAGE is
                           Docker Hub's regclient/regctl pinned by digest.
  the PKGBUILD             renders for a release (build/ci/channels/aur-publish.py):
                           the release's tarball from the package registry, one
                           pkgver, pkgrel and sha256sums line each, and
                           -DHM_LIVE_UPDATE=OFF.
  aur.known_hosts          the AUR's three host keys, at the fingerprints it publishes.
  obs/_service             the group's permanent link to the latest tarball, and no
                           version.
  .copr/Makefile           runs copr-build.py srpm, and .gitattributes checks it out LF,
                           which GNU make in Copr's builder needs.

Usage:
  python3 build/check-channels.py [--root DIR]
  python3 build/check-channels.py --self-test   breaks each rule in a copy
"""

import argparse
import importlib.util
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CI = os.path.join(".gitlab", "ci", "channels.yml")
CHANNELS = os.path.join("build", "ci", "channels")
PUBLISH = ["channels-verify", "registry-mirror", "aur-publish", "ppa-upload", "copr-build", "obs-publish",
           "chocolatey-release", "snap-upload"]
FOLLOWS = {"chocolatey-publish": "chocolatey-release"}
# The jobs in a docker image, on linix or server2 (default:'s tags, or snap-upload's own
# linix) - GitLab's shared runners until 9 October 2026 - none of whose runners maps
# GitHub's hosts itself: each keeps GitHub out of itself (.gitlab/ci/no-github.yml).
NO_GITHUB_JOBS = ["channels-check", "channels-verify", "registry-mirror", "aur-publish", "ppa-upload", "copr-build",
                  "obs-publish", "chocolatey-release", "snap-upload"]
PUBLISH_RULES = '''.channels-publish:
  rules:
    - if: $CI_PIPELINE_SOURCE == "schedule" || $CI_PIPELINE_SOURCE == "pipeline" || $CI_PIPELINE_SOURCE == "merge_request_event"
      when: never
    - if: $CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH && $CI_COMMIT_REF_PROTECTED == "true"
      when: manual
      allow_failure: true
'''
FOLLOW_RULES = '''  rules:
    - if: $CI_PIPELINE_SOURCE == "schedule" || $CI_PIPELINE_SOURCE == "pipeline" || $CI_PIPELINE_SOURCE == "merge_request_event"
      when: never
    - if: $CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH && $CI_COMMIT_REF_PROTECTED == "true"
      allow_failure: true
'''


def load(name, path):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def read(root, relative):
    with open(os.path.join(root, relative), encoding="utf-8") as f:
        return f.read().replace("\r\n", "\n")


def jobs(text):
    """{name: its block} of the file's top-level keys."""
    blocks, name, lines = {}, None, []
    for line in text.split("\n"):
        match = re.match(r"^([A-Za-z0-9_.\-]+):\s*$", line)
        if match:
            if name:
                blocks[name] = "\n".join(lines) + "\n"
            name, lines = match.group(1), [line]
        elif name:
            lines.append(line)
    if name:
        blocks[name] = "\n".join(lines) + "\n"
    return blocks


def problems_of(root):
    problems = []
    try:
        text = read(root, CI)
    except OSError:
        return ["%s is not there" % CI]
    blocks = jobs(text)
    if PUBLISH_RULES not in text:
        problems.append("%s's .channels-publish is not the protected master's own pipeline alone, by hand, allowed to fail"
                        % CI)
    for name in PUBLISH + list(FOLLOWS):
        block = blocks.get(name)
        if block is None:
            problems.append("%s has no job %s" % (CI, name))
            continue
        if name in FOLLOWS:
            if FOLLOW_RULES not in block or "when: manual" in block:
                problems.append("%s's rules are not a protected master's own pipeline alone" % name)
            if not re.search(r"needs:\n    - job: %s\n      artifacts: true\n" % re.escape(FOLLOWS[name]), block):
                problems.append("%s does not follow %s with its artifacts" % (name, FOLLOWS[name]))
        elif "    - !reference [.channels-publish, rules]\n" not in block or re.search(r"^    - if:", block, re.MULTILINE):
            problems.append("%s's rules are not .channels-publish's alone" % name)
        if re.search(r"^\s+(id_tokens|secrets):", block, re.MULTILINE):
            problems.append("%s asks for id_tokens or secrets" % name)
        scripts = re.findall(r"build[/\\]ci[/\\]channels[/\\]([A-Za-z0-9\-]+\.(?:py|ps1))", block)
        if not scripts:
            problems.append("%s runs no script of %s" % (name, CHANNELS.replace(os.sep, "/")))
        for script in scripts:
            if not os.path.isfile(os.path.join(root, CHANNELS, script)):
                problems.append("%s runs build/ci/channels/%s, which is not there" % (name, script))
    arch_hosts = '    - "sed -i \'s/^hosts:.*/hosts: files dns/\' /etc/nsswitch.conf"'
    for name in NO_GITHUB_JOBS:
        # The before_script's first command; or its second, after the one line that
        # makes Arch Linux's image read its hosts file first.
        parts = (blocks.get(name) or "").split("  before_script:\n", 1)
        steps = [l for l in parts[1].split("\n") if l.startswith("    - ")] if len(parts) == 2 else []
        first = [s for s in steps[:2] if s != arch_hosts]
        if not first or first[0] != "    - !reference [.no-github, before_script]":
            problems.append("%s, in a docker image on a runner that maps no GitHub host, does not take .no-github "
                            "first" % name)
    rf = load("release_files", os.path.join(root, CHANNELS, "release-files.py"))
    try:
        rf.pinned_image("regctl", {}, root)
    except rf.Refused as error:
        problems.append(str(error))

    aur = load("aur_publish", os.path.join(root, CHANNELS, "aur-publish.py"))
    try:
        rendered = aur.render(read(root, aur.PKGBUILD), "9.9.9", "ab" * 32)
        if not re.search(r"^\s+-DHM_LIVE_UPDATE=OFF \\$", rendered, re.MULTILINE):
            problems.append("the PKGBUILD builds the live update: a distribution's build has it off")
    except aur.Refused as error:
        problems.append(str(error))
    try:
        aur.known_hosts_lines(os.path.join(root, CHANNELS, "aur.known_hosts"))
    except (aur.Refused, OSError) as error:
        problems.append(str(error))

    try:
        service = read(root, os.path.join("hmailserver", "source", "Server", "platform", "packaging", "obs", "_service"))
        if "https://gitlab.com/hmailserver/hmailserver/-/releases/permalink/latest/downloads/hmailserver.tar.gz" not in service:
            problems.append("obs/_service does not fetch the group's permanent link to the latest release's tarball")
    except OSError:
        problems.append("hmailserver/source/Server/platform/packaging/obs/_service is not there")
    try:
        makefile = read(root, os.path.join(".copr", "Makefile"))
        if not re.search(r"^\tpython3 \$\(top\)/build/ci/channels/copr-build\.py srpm --outdir \"\$\(outdir\)\"$", makefile,
                         re.MULTILINE):
            problems.append(".copr/Makefile's srpm does not run copr-build.py srpm into $(outdir)")
    except OSError:
        problems.append(".copr/Makefile is not there")
    if not re.search(r"^\.copr/\*\* text eol=lf$", read(root, ".gitattributes"), re.MULTILINE):
        problems.append(".gitattributes does not check .copr/ out LF: GNU make in Copr's builder would read carriage returns")
    return problems


def self_test():
    failures = []
    # regctl's release as the channels' file pins it: Renovate moves it (renovate.json's
    # HM_*_IMAGE manager), so the case below breaks what is there. Not found, the case's
    # text is missing, and the case fails.
    with open(os.path.join(ROOT, CI), encoding="utf-8") as f:
        regctl = re.search(r'HM_REGCTL_IMAGE: "docker\.io/regclient/regctl:(v[0-9][A-Za-z0-9._\-]*)@sha256:', f.read())
    regctl = regctl.group(1) if regctl else "HM_REGCTL_IMAGE not found"
    names = [CI, os.path.join(".copr", "Makefile"), ".gitattributes",
             os.path.join("hmailserver", "source", "Server", "platform", "packaging", "PKGBUILD"),
             os.path.join("hmailserver", "source", "Server", "platform", "packaging", "obs", "_service"),
             os.path.join(".gitlab-ci.yml")]
    cases = [
        ("a publish job for tags too", CI, '    - if: $CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH && $CI_COMMIT_REF_PROTECTED == "true"\n'
         '      when: manual', '    - if: $CI_COMMIT_REF_PROTECTED == "true"\n      when: manual', "not the protected master"),
        ("a job with rules of its own", CI, "  rules:\n    - !reference [.channels-publish, rules]\n  environment:\n    name: aur\n",
         "  rules:\n    - !reference [.channels-publish, rules]\n    - if: $CI_COMMIT_TAG\n  environment:\n    name: aur\n",
         "aur-publish's rules"),
        ("a job with an ID token", CI, "  environment:\n    name: copr\n", "  id_tokens:\n    X:\n      aud: x\n  environment:\n    name: copr\n",
         "copr-build asks for id_tokens"),
        ("a script that is not there", CI, "build/ci/channels/obs-publish.py --out", "build/ci/channels/obs-publsh.py --out",
         "obs-publsh.py, which is not there"),
        ("chocolatey-publish pressed by hand", CI, "  resource_group: chocolatey-publish\n",
         "  resource_group: chocolatey-publish\n  when: manual\n", "chocolatey-publish's rules"),
        ("a docker job without .no-github", CI,
         "  before_script:\n    - !reference [.no-github, before_script]\n  script:\n    - python3 build/ci/channels/obs-publish.py",
         "  script:\n    - python3 build/ci/channels/obs-publish.py", "obs-publish, in a docker image"),
        ("regctl by tag alone", CI, "regctl:%s@sha256:" % regctl, "regctl:%s-x@sha25:" % regctl, "pinned by digest"),
        ("the PKGBUILD from a git tag", names[3], 'source=("https://gitlab.com/api/v4/projects/86730559/packages/generic/'
         'hmailserver/${pkgver}/hmailserver-${pkgver}.tar.gz")', 'source=("git+${url}.git#tag=v${pkgver}")', "source="),
        ("the PKGBUILD with the live update", names[3], "      -DHM_LIVE_UPDATE=OFF \\\n", "", "builds the live update"),
        ("OBS from the old path", names[4], "https://gitlab.com/hmailserver/hmailserver/-/releases/",
         "https://gitlab.com/Progressiverobot/hmailserver/-/releases/", "permanent link"),
        ("the Makefile elsewhere", names[1], "\tpython3 $(top)/build/ci/channels/copr-build.py srpm",
         "\tpython3 $(top)/build/ci/channels/copr-build.py trigger", "does not run copr-build.py srpm"),
        (".copr checked out CRLF", names[2], ".copr/** text eol=lf", ".copr/** text", "does not check .copr/ out LF"),
    ]
    work = tempfile.mkdtemp(prefix="hm-check-channels-")
    try:
        for relative in names + [os.path.join(CHANNELS, n) for n in os.listdir(os.path.join(ROOT, CHANNELS))
                                 if os.path.isfile(os.path.join(ROOT, CHANNELS, n))] + [
                os.path.join("build", "ci", "cli-packages.py"), os.path.join("build", "check-cli-packages.py"),
                os.path.join("build", "ci", "release-chain.py"), os.path.join("build", "ci", "sigstore-certificate.py")]:
            target = os.path.join(work, "tree", relative)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copyfile(os.path.join(ROOT, relative), target)
        tree = os.path.join(work, "tree")
        base = problems_of(tree)
        print("%s  the tree as it is: %s" % ("ok  " if not base else "FAIL", "; ".join(base) or "no problem"))
        if base:
            failures.append("base")
        for what, relative, old, new, needle in cases:
            path = os.path.join(tree, relative)
            with open(path, encoding="utf-8", newline="") as f:
                held = f.read()
            text = held.replace("\r\n", "\n")
            if old not in text:
                print("FAIL  %s: the case's text is not in %s" % (what, relative))
                failures.append(what)
                continue
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(text.replace(old, new, 1))
            found = problems_of(tree)
            caught = any(needle in p for p in found)
            print("%s  %s (%s)" % ("ok  " if caught else "FAIL", what, ([p for p in found if needle in p] or found or
                                                                         ["not caught"])[0][:100]))
            if not caught:
                failures.append(what)
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(held)
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
    parser.add_argument("--root", default=ROOT)
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    problems = problems_of(os.path.abspath(arguments.root))
    for problem in problems:
        print("PROBLEM %s" % problem)
    if problems:
        return 1
    print("The release channels' files are what hmailserver/docs/Channels.md says.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
