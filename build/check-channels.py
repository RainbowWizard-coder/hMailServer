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
                           Docker Hub's regclient/regctl pinned by digest;
                           galaxy-publish needs galaxy-build with its artifacts, the
                           tarball it sends; galaxy-check, the one check job beside
                           channels-check, never runs for a tag or a schedule; each
                           Galaxy job's rules are its one reference and nothing
                           more; and the three install their tools with their hashes
                           alone, no command of theirs installing anything else with
                           an installer GALAXY_INSTALLER names - read line by line
                           and with the lines a backslash or a plain scalar goes on
                           over joined.
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
           "chocolatey-release", "snap-upload", "galaxy-build", "galaxy-publish"]
FOLLOWS = {"chocolatey-publish": "chocolatey-release"}
# The jobs in a docker image, on linix, server2 or s1-linux (default:'s tags, or snap-upload's own
# linix) - GitLab's shared runners until 9 October 2026 - none of whose runners maps
# GitHub's hosts itself: each keeps GitHub out of itself (.gitlab/ci/no-github.yml).
NO_GITHUB_JOBS = ["channels-check", "channels-verify", "registry-mirror", "aur-publish", "ppa-upload", "copr-build",
                  "obs-publish", "chocolatey-release", "snap-upload", "galaxy-check", "galaxy-build", "galaxy-publish"]
GALAXY_CHANGES = '''.galaxy-changes:
  rules:
    - if: $CI_PIPELINE_SOURCE == "schedule" || $CI_PIPELINE_SOURCE == "pipeline"
      when: never
    - if: $CI_COMMIT_TAG
      when: never
    - changes:
'''
GALAXY_PIP = ("/tmp/galaxy/bin/pip install --quiet --disable-pip-version-check --no-cache-dir --require-hashes"
              " -r build/ci/ansible-collection-requirements.txt")
# A command of a Galaxy job that installs something from an index with one of the
# installers named here, in a job's script, before_script or a block scalar in them:
# pip, pip3 or pip3.14 with general options before install (each with a value or none),
# python -m pip with or without the space, venv's --upgrade-deps (which upgrades the new
# environment's pip from PyPI), uv run, pip, tool, add, sync or venv with options before
# them, uvx, pipx, virtualenv, easy_install, pipenv, poetry, pdm, conda, mamba,
# micromamba, pixi, rye or hatch with install, add, sync, update, upgrade or create
# (hatch's and conda's env create too), ansible-galaxy's install with options anywhere
# before it, and ansible-test with --requirements or --venv, with which it installs its
# tests' tools. Only GALAXY_PIP may: it is read in each line of the job, and in the
# job's text with every line a backslash or a plain scalar's continuation goes on over
# joined to the one before it. It is a scan of the commands as they are written: an
# installer the shell puts together as it runs (ins''tall, a variable) or that Python
# runs from -c is not read.
OPTION = r"\s+-{1,2}[^\s;&|]+"
GALAXY_INSTALLER = re.compile(
    r"\bpip[0-9.]*(?:%s(?:\s+(?!install\b)[^\s;&|-][^\s;&|]*)?)*\s+install\b|-m\s*pip\b"
    r"|--upgrade-deps\b|\bvirtualenv\b"
    r"|\b(?:pipenv|poetry|pdm|conda|mamba|micromamba|pixi|rye|hatch)(?:%s)*\s+(?:env\s+)?"
    r"(?:install|add|sync|update|upgrade|create)\b"
    r"|\buv(?:%s)*\s+(?:run|pip|tool|add|sync|venv)\b|\buvx\b|\bpipx\b|\beasy_install\b"
    r"|\bansible-galaxy(?:%s)*\s+(?:(?:collection|role)(?:%s)*\s+)?install\b"
    r"|\bansible-test\b[^;&|]*?\s--(?:requirements|venv)\b" % (OPTION, OPTION, OPTION, OPTION, OPTION))
# Each Galaxy job's rules, exactly: its one reference, with no rule before or after it.
GALAXY_RULES = {"galaxy-check": ".galaxy-changes", "galaxy-build": ".channels-publish",
                "galaxy-publish": ".channels-publish"}
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


def key_block(block, key):
    """The lines of a job's key, indented two, to the next line indented no deeper, each
    ending in a line feed; empty when the job has no such key."""
    lines, inside = [], False
    for line in block.split("\n"):
        if inside and line.strip() and len(line) - len(line.lstrip(" ")) <= 2:
            break
        if line == "  %s:" % key:
            inside = True
        if inside:
            lines.append(line)
    while lines and not lines[-1].strip():
        lines.pop()
    return "".join(line + "\n" for line in lines)


def galaxy_installs(block):
    """What a Galaxy job's text (jobs()'s) installs but GALAXY_PIP, or how that goes
    wrong; empty when GALAXY_PIP is its one install, a list item of its own that no line
    goes on."""
    job = block.split("\n")
    installs = [re.sub(r"^\s*(?:-\s+)?", "", line) for line in job if GALAXY_INSTALLER.search(line)]
    if installs != [GALAXY_PIP]:
        return installs or ["no pip install"]
    at = [i for i, line in enumerate(job) if line.strip() == "- " + GALAXY_PIP]
    following = next((line for line in job[at[0] + 1:] if line.strip()), "") if len(at) == 1 else ""
    if len(at) != 1 or len(following) - len(following.lstrip(" ")) > len(job[at[0]]) - len(job[at[0]].lstrip(" ")):
        return ["the hashed install, not a list item of its own, or one the next line goes on"]
    # The job's text with each command's lines joined, the hashed install taken out.
    joined = re.sub(r"\s+", " ", re.sub(r"\\\r?\n", " ", block)).replace(GALAXY_PIP, "", 1)
    return [m.group(0) for m in GALAXY_INSTALLER.finditer(joined)]


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
    publish = blocks.get("galaxy-publish") or ""
    if not re.search(r"needs:\n    - job: galaxy-build\n      artifacts: true\n", publish):
        problems.append("galaxy-publish does not need galaxy-build with its artifacts: it would send no tarball galaxy-build"
                        " made and recorded")
    check = blocks.get("galaxy-check")
    if check is None:
        problems.append("%s has no job galaxy-check" % CI)
    else:
        if GALAXY_CHANGES not in text or "    - !reference [.galaxy-changes, rules]\n" not in check or re.search(
                r"^    - if:", check, re.MULTILINE):
            problems.append("galaxy-check's rules are not .galaxy-changes' alone, which never run for a tag or a schedule")
        if re.search(r"^\s+(id_tokens|secrets):", check, re.MULTILINE):
            problems.append("galaxy-check asks for id_tokens or secrets")
    for name, reference in sorted(GALAXY_RULES.items()):
        rules = key_block(blocks.get(name) or "", "rules")
        if rules != "  rules:\n    - !reference [%s, rules]\n" % reference:
            problems.append("%s's rules are not !reference [%s, rules] alone, with no rule before or after it: %s"
                            % (name, reference, (rules or "none").strip().replace("\n", " ")))
    for name in ("galaxy-check", "galaxy-build", "galaxy-publish"):
        found = galaxy_installs(blocks.get(name) or "")
        if found:
            problems.append("%s does not install its tools from build/ci/ansible-collection-requirements.txt with their"
                            " hashes alone: %s" % (name, found))
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
        ("galaxy-publish without galaxy-build's tarball", CI, "  needs:\n    - job: galaxy-build\n      artifacts: true\n",
         "  needs: []\n", "does not need galaxy-build"),
        ("galaxy-publish for every protected branch", CI, "  resource_group: galaxy-publish\n  rules:\n"
         "    - !reference [.channels-publish, rules]\n", "  resource_group: galaxy-publish\n  rules:\n"
         "    - if: $CI_COMMIT_REF_PROTECTED == \"true\"\n      when: manual\n", "galaxy-publish's rules"),
        ("galaxy-check for a tag too", CI, "    - if: $CI_COMMIT_TAG\n      when: never\n    - changes:\n"
         "        - hmailserver/source/Server/platform/packaging/ansible/**/*\n",
         "    - changes:\n        - hmailserver/source/Server/platform/packaging/ansible/**/*\n", "galaxy-check's rules"),
        ("galaxy-check with an ID token", CI, "  rules:\n    - !reference [.galaxy-changes, rules]\n",
         "  rules:\n    - !reference [.galaxy-changes, rules]\n  id_tokens:\n    X:\n      aud: x\n",
         "galaxy-check asks for id_tokens"),
        ("galaxy-build's tools without their hashes", CI, "--no-cache-dir --require-hashes -r build/ci/ansible-collection-"
         "requirements.txt\n  script:\n    - export PATH=\"/tmp/galaxy/bin:$PATH\"\n    - python3 build/ci/channels/"
         "galaxy-publish.py release-build", "--no-cache-dir -r build/ci/ansible-collection-requirements.txt\n  script:\n"
         "    - export PATH=\"/tmp/galaxy/bin:$PATH\"\n    - python3 build/ci/channels/galaxy-publish.py release-build",
         "galaxy-build does not install its tools"),
        ("galaxy-check without .no-github", CI, "    HM_COLLECTION: hmailserver/source/Server/platform/packaging/ansible/"
         "ansible_collections/progressiverobot/hmailserver\n  before_script:\n    - !reference [.no-github, before_script]\n",
         "    HM_COLLECTION: hmailserver/source/Server/platform/packaging/ansible/ansible_collections/progressiverobot/"
         "hmailserver\n  before_script:\n", "galaxy-check, in a docker image"),
        ("galaxy-check with a rule of its own before .galaxy-changes'", CI,
         "  rules:\n    - !reference [.galaxy-changes, rules]\n",
         "  rules:\n    - when: always\n    - !reference [.galaxy-changes, rules]\n", "galaxy-check's rules"),
        ("galaxy-check with a changes rule before .galaxy-changes', true for a tag", CI,
         "  rules:\n    - !reference [.galaxy-changes, rules]\n",
         "  rules:\n    - changes: [LICENSE]\n    - !reference [.galaxy-changes, rules]\n", "galaxy-check's rules"),
        ("galaxy-build pressed for any pipeline", CI, "  rules:\n    - !reference [.channels-publish, rules]\n  before_script:\n"
         "    - !reference [.no-github, before_script]\n    - python3 -m venv /tmp/galaxy\n",
         "  rules:\n    - when: manual\n    - !reference [.channels-publish, rules]\n  before_script:\n"
         "    - !reference [.no-github, before_script]\n    - python3 -m venv /tmp/galaxy\n", "galaxy-build's rules"),
        ("galaxy-publish pressed for any pipeline", CI, "  resource_group: galaxy-publish\n  rules:\n"
         "    - !reference [.channels-publish, rules]\n", "  resource_group: galaxy-publish\n  rules:\n"
         "    - when: manual\n    - !reference [.channels-publish, rules]\n", "galaxy-publish's rules"),
        ("galaxy-check installing a tool with pip3", CI, "    - python3 -m venv /tmp/galaxy\n",
         "    - python3 -m venv /tmp/galaxy\n    - pip3 install yamllint\n", "galaxy-check does not install"),
        ("galaxy-check installing a tool inside its script's block", CI, "      cd \"$HM_COLLECTION\"\n",
         "      cd \"$HM_COLLECTION\"\n      /tmp/galaxy/bin/pip install yamllint\n", "galaxy-check does not install"),
        ("galaxy-build installing a collection from Galaxy", CI,
         "    - python3 build/ci/channels/galaxy-publish.py release-build --out galaxy-build\n",
         "    - ansible-galaxy collection install community.general\n"
         "    - python3 build/ci/channels/galaxy-publish.py release-build --out galaxy-build\n",
         "galaxy-build does not install"),
    ]
    # galaxy_installs() over galaxy-check's text with one change each, in memory: each
    # must be found. The tree's text must give nothing.
    script = "      cd \"$HM_COLLECTION\"\n"
    hashed = "    - " + GALAXY_PIP + "\n"
    install_cases = [("installing a tool with " + what, script, script + line) for what, line in (
        ("pip --quiet before install", "      /tmp/galaxy/bin/pip --quiet install yamllint\n"),
        ("pip -q before install", "      /tmp/galaxy/bin/pip -q install yamllint\n"),
        ("pip --index-url and its value before install",
         "      /tmp/galaxy/bin/pip --index-url https://pypi.org/simple install yamllint\n"),
        ("python -mpip", "      /tmp/galaxy/bin/python -mpip install yamllint\n"),
        ("pip, its install on the next line after a backslash", "      /tmp/galaxy/bin/pip \\\n        install yamllint\n"),
        ("uv run --with", "      uv run --with yamllint yamllint .\n"),
        ("uv --quiet pip install", "      uv --quiet pip install yamllint\n"),
        ("ansible-galaxy -v collection install", "      ansible-galaxy -v collection install community.general\n"),
        ("ansible-test sanity --requirements", "      ansible-test sanity --requirements --python 3.14\n"),
        ("ansible-test sanity --venv", "      ansible-test sanity --venv --python 3.14\n"),
        ("pipenv install", "      pipenv install yamllint\n"),
        ("poetry add", "      poetry add yamllint\n"),
        ("pdm add", "      pdm add yamllint\n"),
        ("conda install -y", "      conda install -y yamllint\n"),
        ("mamba --quiet create", "      mamba --quiet create -n lint yamllint\n"),
        ("micromamba install", "      micromamba install yamllint\n"),
        ("uv venv --seed", "      uv venv --seed /tmp/lint\n"),
        ("virtualenv", "      virtualenv /tmp/lint\n"))] + [
        ("its environment made with venv's --upgrade-deps", "    - python3 -m venv /tmp/galaxy\n",
         "    - python3 -m venv --upgrade-deps /tmp/galaxy\n"),
        ("its hashed install given a further requirement on the next line", hashed, hashed + "      yamllint\n"),
        ("installing a tool over two list lines", hashed, hashed + "    - /tmp/galaxy/bin/pip\n      install yamllint\n"),
        ("its hashed install twice", hashed, hashed + hashed)]
    galaxy_check = jobs(read(ROOT, CI)).get("galaxy-check") or ""
    print("%s  galaxy-check's installs as they are: %s" % (("ok  ", "nothing but the hashed file") if not galaxy_installs(
        galaxy_check) else ("FAIL", galaxy_installs(galaxy_check))))
    if galaxy_installs(galaxy_check):
        failures.append("galaxy-check's installs")
    for what, old, new in install_cases:
        found = galaxy_installs(galaxy_check.replace(old, new, 1)) if old in galaxy_check else []
        print("%s  galaxy-check %s (%s)" % ("ok  " if found else "FAIL", what, (found or ["not caught"])[-1][:80]))
        if not found:
            failures.append(what)
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
