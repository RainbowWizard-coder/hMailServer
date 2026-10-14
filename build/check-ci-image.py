#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Linux CI jobs run on the image their Dockerfile describes, and no older one -
and, once it is in the project's registry, on that image's bytes and no others.

The Linux jobs of .gitlab-ci.yml (linux-build, linux-suite and every job that
extends .linix) run on the project's own runners with the tag linix - linix,
server2 and s1-linux - in an image built on linix by hand from build/ci/linux.Dockerfile
(build/ci/build-linux-image.sh) and, since 6 October 2026, pushed to the project's
registry and pulled from there. So an edit to the Dockerfile changes nothing a job
runs on until somebody builds it - and a job that went on naming the old tag would
pass or fail on the old image and say nothing about it.

The tag is therefore made from the file: <release>-<hash>, where <release> is the
Ubuntu release of the FROM line and <hash> the first twelve hex digits of the
SHA-256 of the Dockerfile with its line endings made LF (the tree checks some files
out CRLF; this one is LF by .gitattributes, and the hash must not depend on
either). .gitlab-ci.yml names the image in one of two ways:

  hmailserver-ci/linux:<tag>
      the tag in linix's own image store, found there and never pulled. A tag names
      whatever was last given it: any job that is root on linix - the privileged
      linix-systemd entry's are - could give it other bytes, and every later job,
      a release's packages among them, would build in those.

  registry.gitlab.com/hmailserver/hmailserver/ci-linux:<tag>@sha256:<digest>
      the same image pushed to the project's container registry
      (build/ci/build-linux-image.sh --push), named by the digest of its manifest.
      Docker takes an image named so only as the registry holds that digest: a
      tag given to other bytes on linix is not it, and is not run (root on linix
      rewriting Docker's store underneath is beyond any check a job can make).
      The tag beside the digest says which Dockerfile it was built from, and is
      held to it below. (The second security review of 6.3.6, SUP2-1.)

This checker fails when:

  * .gitlab-ci.yml names neither form, names one whose tag is not the tag of the
    Dockerfile as it is now - the Dockerfile was edited and the name was not; the
    message says the tag to write and the command that builds it - names both forms,
    or names the registry form without a whole sha256 digest, or with two digests.
    The files .gitlab-ci.yml includes from .gitlab/ci/ are held to the same rules,
    so that a job there cannot name the image another way;
  * once the registry form is named, .linix's before_script is not the --job step
    below, or a job runs in the image without starting with the step: one that takes
    .linix's image (image: !reference [.linix, image]) without its before_script
    (before_script: !reference [.linix, before_script]), or one that extends .linix
    or such a job with a before_script of its own that leaves the step out; or a job
    that extends .linix keeps the step in an image of its own, where it would refuse;
  * a FROM line of the Dockerfile is not pinned by digest, which would let the
    same tag stand for two different images on two days;
  * build/ci/build-linux-image.sh --push no longer keeps the digest the registry's
    tag named before the push. A rebuild from an unchanged Dockerfile moves the tag
    to the new manifest, and GitLab.com's registry removes an untagged manifest
    that nothing references about 24 hours later, while master and every open
    batch still pin the old digest; so the script, before it pushes, reads the
    digest the tag names (skopeo inspect --raw, hashed), gives it the dated tag
    <tag>-<yyyymmdd> by a copy that preserves the digest (skopeo copy --all
    --preserve-digests, the login's file mounted read-only for the destination
    alone), reads that back and holds it to the digest, and refuses a login kept
    in a credential helper. Each step is held to the script's text, in that order;
    the self-test runs the script with a docker of its own and a registry of files.

A job whose image has not been built on linix (or pushed) fails at its first step,
naming the image it could not find: loud, and in the right place.

  --job        run as a job's first step: the image this job runs in (CI_JOB_IMAGE,
               which GitLab sets from the job's image: name) must be the registry
               form, named by digest, with the tag of this checkout's Dockerfile.
               A job of a release that starts from an image named by tag alone
               stops here, before it builds anything.
  --self-test  each rule broken in memory, and the check made to say so; and
               build/ci/build-linux-image.sh --push run, with a docker of its own
               first on the PATH, through a rebuild that keeps the previous digest,
               a tag new to the registry, a dated tag already taken, a registry that
               cannot be read, a copy that did not preserve the digest and a login
               the copy could not use.

Usage:  python build/check-ci-image.py [--job | --self-test]
"""

import datetime
import hashlib
import io
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCKERFILE = os.path.join(ROOT, "build", "ci", "linux.Dockerfile")
CI_FILE = os.path.join(ROOT, ".gitlab-ci.yml")
CI_DIR = os.path.join(ROOT, ".gitlab", "ci")
SCRIPT = os.path.join(ROOT, "build", "ci", "build-linux-image.sh")

LOCAL = "hmailserver-ci/linux"
REGISTRY = "registry.gitlab.com/hmailserver/hmailserver/ci-linux"
SKOPEO = "quay.io/skopeo/stable:v1.20.0"

# What build/ci/build-linux-image.sh --push does before the push moves the tag, each
# as the script writes it, in the order the script must keep them: the digest the tag
# names read first, the dated tag made from the UTC date, the copy that preserves the
# digest with the login for the destination alone, the read-back held to the digest,
# then the push. And the login's file: mounted read-only, refused when it names a
# credential helper the container cannot reach.
SCRIPT_STEPS = (
    ('previous=$(registry_digest "$remote")', "reads the digest the registry's tag names before the push"),
    ('today=$(date -u +%Y%m%d)', "the dated tag's date, UTC"),
    ('kept="${release}-${digest}-${today}"', "the dated tag, <tag>-<yyyymmdd>"),
    ('kept="${release}-${digest}-${today}-${n}"', "the next free dated tag of the same day, -<yyyymmdd>-2 on"),
    ('copy --quiet --all --preserve-digests', "a copy that preserves the digest"),
    ('--dest-authfile "$mounted" "docker://$registry@$previous" "docker://$registry:$kept"',
     "from the digest to the dated tag, with the login for the destination alone"),
    ('copied=$(registry_digest "$registry:$kept")', "reads the dated tag back"),
    ('if [ "$copied" != "$previous" ]; then', "and holds it to the digest"),
    ('docker push "$remote"', "the push"),
)
SCRIPT_LOGIN = (
    ('-v "$login:$mounted:ro"', "the login's file mounted read-only"),
    ("""grep -Eq '"(credsStore|credHelpers)"' "$login\"""", "a login kept in a credential helper refused"),
    ("""grep -Eq 'manifest unknown|name unknown' "$err\"""", "a name the registry does not hold read as nothing"),
)

LOCAL_IMAGE = re.compile(r"hmailserver-ci/linux:[A-Za-z0-9_.\-]+")
REGISTRY_IMAGE = re.compile(re.escape(REGISTRY) + r"(?::[A-Za-z0-9_.\-]+)?(?:@[A-Za-z0-9]+:[A-Za-z0-9]*)?")
PINNED = re.compile(re.escape(REGISTRY) + r":([A-Za-z0-9_.\-]+)@sha256:([0-9a-f]{64})$")
FROM = re.compile(r"^FROM\s+(\S+)", re.MULTILINE)
UBUNTU = re.compile(r"^FROM\s+ubuntu:(\d+\.\d+)@sha256:[0-9a-f]{64}\b", re.MULTILINE)

# The step every job in the image starts with once the image is named by digest, and
# the two ways a CI file says a job takes .linix's image and its before_script without
# extending .linix.
JOB_STEP = "build/check-ci-image.py --job"
TOP_LEVEL = re.compile(r"^(?![#\s-])([^:]+?):(?:\s|$)")
TAKES_IMAGE = re.compile(r"\bimage:\s*!reference\s*\[\s*[\"']?\.linix[\"']?\s*,\s*[\"']?image[\"']?\s*\]")
TAKES_STEP = re.compile(r"!reference\s*\[\s*[\"']?\.linix[\"']?\s*,\s*[\"']?before_script[\"']?\s*\]")


def expected_tag(raw):
    """The tag the Dockerfile's bytes make, as <release>-<hash>, or None when it has no
    pinned Ubuntu FROM."""
    text = raw.replace(b"\r\n", b"\n")
    release = UBUNTU.search(text.decode("utf-8"))
    if not release:
        return None
    digest = hashlib.sha256(text).hexdigest()[:12]
    return "%s-%s" % (release.group(1), digest)


def dockerfile_problems(raw):
    problems = []
    text = raw.decode("utf-8").replace("\r\n", "\n")
    for image in FROM.findall(text):
        if "@sha256:" not in image:
            problems.append("build/ci/linux.Dockerfile: FROM %s is not pinned by digest (image@sha256:...)." % image)
    if expected_tag(raw) is None:
        problems.append("build/ci/linux.Dockerfile: no 'FROM ubuntu:<release>@sha256:<digest>' line to take the release from.")
    return problems


def named_images(ci_text):
    """The image's names in .gitlab-ci.yml outside its comment lines: (on linix, in
    the registry)."""
    code = "\n".join(line for line in ci_text.splitlines() if not line.lstrip().startswith("#"))
    return sorted(set(LOCAL_IMAGE.findall(code))), sorted(set(REGISTRY_IMAGE.findall(code)))


def ci_problems(ci_text, tag):
    """What is wrong with the names .gitlab-ci.yml gives the image, for the
    Dockerfile's tag (None when the Dockerfile has none)."""
    problems = []
    local, remote = named_images(ci_text)

    if not local and not remote:
        return [".gitlab-ci.yml names no hmailserver-ci/linux image and no %s one." % REGISTRY]
    if local and remote:
        problems.append(".gitlab-ci.yml names the image both by its tag on linix (%s) and in the registry (%s): one image,"
                        " by one name - every Linux job by digest once the image is pushed." % (", ".join(local), ", ".join(remote)))

    for image in local:
        if tag is not None and image != "%s:%s" % (LOCAL, tag):
            problems.append(
                ".gitlab-ci.yml names %s, but build/ci/linux.Dockerfile as it is now makes %s:%s.\n"
                "      Write %s:%s in .gitlab-ci.yml, and build it on linix before the pipeline runs:\n"
                "      bash build/ci/build-linux-image.sh" % (image, LOCAL, tag, LOCAL, tag))

    digests = set()
    for image in remote:
        pinned = PINNED.match(image)
        if not pinned:
            problems.append(".gitlab-ci.yml names %s, which is not %s:<tag>@sha256:<64 hex digits>: an image in the"
                            " registry is named by its digest, or a re-tag could stand for it." % (image, REGISTRY))
            continue
        digests.add(pinned.group(2))
        if tag is not None and pinned.group(1) != tag:
            problems.append(
                ".gitlab-ci.yml names %s, whose tag is not %s, the tag build/ci/linux.Dockerfile as it is now makes.\n"
                "      Build and push it on linix, and write the line the script prints:\n"
                "      bash build/ci/build-linux-image.sh --push" % (image, tag))
    if len(digests) > 1:
        problems.append(".gitlab-ci.yml names the registry image by %d digests; one image, by one digest." % len(digests))
    return problems


def job_problems(job_image, tag):
    """What is wrong with the image a job runs in, as GitLab names it in CI_JOB_IMAGE."""
    if not job_image:
        return ["CI_JOB_IMAGE is not set: this runs as a job's first step, in GitLab CI."]
    pinned = PINNED.match(job_image.strip())
    if not pinned:
        return ["this job runs in %s, which is not %s:<tag>@sha256:<digest>: an image named by its tag alone is"
                " whatever was last given that tag on the runner, and a release is not built in it"
                " (docs/ContinuousIntegration.md, The image)." % (job_image, REGISTRY)]
    if tag is not None and pinned.group(1) != tag:
        return ["this job runs in %s, built from another build/ci/linux.Dockerfile than this commit's, whose tag is %s."
                % (job_image, tag)]
    return []


def blocks(text):
    """The top-level keys of a CI file, each with the lines under it, comment lines left
    out: [(key, code)]."""
    found = []
    for line in text.splitlines():
        top = TOP_LEVEL.match(line)
        if top:
            found.append([top.group(1).strip("'\""), []])
        elif found and not line.lstrip().startswith("#"):
            found[-1][1].append(line)
    return [(key, "\n".join(lines)) for key, lines in found]


def extends_of(code):
    """The names a job's extends: gives, on its line ("extends: a", "extends: [a, b]")
    or as the list under it."""
    names, listing = [], False
    for line in code.splitlines():
        own = re.match(r"^(\s+)extends:\s*(.*?)\s*$", line)
        if own:
            value = own.group(2)
            listing = not value
            names += [n.strip(" '\"") for n in value.strip("[]").split(",") if n.strip(" '\"")]
            continue
        if listing:
            item = re.match(r"^\s+-\s*(\S+)\s*$", line)
            if item:
                names.append(item.group(1).strip("'\""))
            elif line.strip():
                listing = False
    return names


def step_problems(files):
    """With the image named by digest: what lets a job start in it without the --job
    step, or gives the step to a job in another image. files: [(path, text)] -
    .gitlab-ci.yml and the files it includes. A job is in the image when it takes
    .linix's image (image: !reference [.linix, image]) or extends one that is and names
    no image of its own; it starts with the step when its own before_script has it
    (the step, or !reference [.linix, before_script]) or, having none of its own, it
    extends one that does - a before_script of its own replaces the one it extends."""
    jobs = {}
    for path, text in files:
        for key, code in blocks(text):
            jobs[key] = {
                "path": path,
                "extends": extends_of(code),
                "takes_image": bool(TAKES_IMAGE.search(code)),
                "own_image": bool(re.search(r"^\s+image:", code, re.MULTILINE)),
                "own_steps": bool(re.search(r"^\s+before_script:", code, re.MULTILINE)),
                "job_step": JOB_STEP in code,
                "step": JOB_STEP in code or bool(TAKES_STEP.search(code)),
            }

    def walk(key, field, seen=()):
        job = jobs.get(key)
        if job is None or key in seen:
            return False
        if field == "image":
            if key == ".linix" or job["takes_image"]:
                return True
            if job["own_image"]:
                return False
        else:
            if job["own_steps"]:
                return job["step"]
        return any(walk(parent, field, seen + (key,)) for parent in job["extends"])

    problems = []
    if ".linix" not in jobs:
        return ["no CI file defines .linix, the template the Linux jobs take the image from."]
    if not (jobs[".linix"]["own_steps"] and jobs[".linix"]["job_step"]):
        problems.append(".linix names the image by digest, but its before_script is not 'python3 %s': a job would"
                        " build in the image without asking whether it is the one named." % JOB_STEP)
    for key in sorted(jobs):
        if key == ".linix":
            continue
        job = jobs[key]
        in_image, stepped = walk(key, "image"), walk(key, "step")
        if in_image and not stepped:
            problems.append("%s: %s runs in .linix's image without starting with 'python3 %s' - %s" % (
                job["path"], key, JOB_STEP,
                "begin its own before_script with '- !reference [.linix, before_script]'" if job["own_steps"]
                else "add 'before_script: !reference [.linix, before_script]'"))
        elif stepped and not in_image and any(walk(parent, "image") for parent in job["extends"]):
            problems.append("%s: %s names an image of its own but keeps .linix's before_script, whose --job step"
                            " refuses every image but .linix's - give it a before_script of its own." % (job["path"], key))
    return problems


def ci_files():
    """.gitlab-ci.yml and every file under .gitlab/ci/ it may include: [(path, text)]."""
    files = []
    with io.open(CI_FILE, "r", encoding="utf-8") as f:
        files.append((".gitlab-ci.yml", f.read()))
    if os.path.isdir(CI_DIR):
        for name in sorted(os.listdir(CI_DIR)):
            if name.endswith((".yml", ".yaml")):
                with io.open(os.path.join(CI_DIR, name), "r", encoding="utf-8") as f:
                    files.append((".gitlab/ci/" + name, f.read()))
    return files


def tree_problems(raw, files):
    """Everything this checker holds a commit to: the Dockerfile, the names every CI file
    gives the image, and - once it is named by digest - the --job step."""
    tag = expected_tag(raw)
    ci_text = "\n".join(text for _, text in files)
    problems = dockerfile_problems(raw) + ci_problems(ci_text, tag)
    if named_images(ci_text)[1]:
        problems += step_problems(files)
    return problems


def script_problems(text):
    """What build/ci/build-linux-image.sh no longer does of SCRIPT_STEPS, in their order,
    and of SCRIPT_LOGIN."""
    problems = []
    last = -1
    for needle, why in SCRIPT_STEPS:
        at = text.find(needle)
        if at < 0:
            problems.append("build/ci/build-linux-image.sh no longer has %s (%s)" % (needle, why))
        elif at < last:
            problems.append("build/ci/build-linux-image.sh has %s (%s) before the step it must follow: the digest is read,"
                            " kept, read back, and only then pushed" % (needle, why))
        else:
            last = at
    for needle, why in SCRIPT_LOGIN:
        if needle not in text:
            problems.append("build/ci/build-linux-image.sh no longer has %s (%s)" % (needle, why))
    return problems


def bash_program():
    """The bash that runs the script in the self-test: on Windows, Git's own (found
    from git's exec path, or at its usual place), never System32's, which is WSL's;
    elsewhere the one on the PATH. None when there is none."""
    if sys.platform != "win32":
        return shutil.which("bash")
    roots = [r"C:\Program Files\Git"]
    git = shutil.which("git")
    if git:
        try:
            exec_path = subprocess.run([git, "--exec-path"], capture_output=True, text=True, check=True).stdout.strip()
            roots.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(exec_path))))
        except (OSError, subprocess.CalledProcessError):
            pass
    for root in roots:
        for candidate in (os.path.join(root, "bin", "bash.exe"), os.path.join(root, "usr", "bin", "bash.exe")):
            if os.path.isfile(candidate):
                return candidate
    found = shutil.which("bash")
    if found and "system32" not in found.lower():
        return found
    return None


# The docker of the self-test, first on the script's PATH: it answers build, tag, push,
# image inspect and image ls, and skopeo's inspect --raw and copy, from the files under
# $FAKE - the registry's tags as files under registry/, each holding its manifest's
# bytes; new-manifest, what the push gives the tag; carries, what the built image's
# RepoDigests already hold; others, the other tags on the host; down, present when the
# registry answers nothing; corrupt-copy, present when the copy alters the bytes -
# and records every call, one line each, in calls.
FAKE_DOCKER = r'''#!/bin/bash
set -eu
reg="$FAKE/registry"
printf '%s\n' "$*" >> "$FAKE/calls"
digest_of() { printf 'sha256:%s' "$(sha256sum "$1" | cut -d' ' -f1)"; }
by_digest() {
  local f
  for f in "$reg"/*; do
    [ -f "$f" ] || continue
    if [ "$(digest_of "$f")" = "$1" ]; then printf '%s' "$f"; return; fi
  done
}
unknown() {
  echo "time=\"2026-10-08T00:00:00Z\" level=fatal msg=\"Error parsing image name \\\"docker://$1\\\": reading manifest ${1##*:} in ${1%%:*}: manifest unknown\"" >&2
  exit 2
}
case "$1" in
  build|tag) exit 0 ;;
  push)
    cp "$FAKE/new-manifest" "$reg/${2##*:}"
    digest_of "$FAKE/new-manifest" > "$FAKE/pushed"
    echo "${2##*:}: digest: $(cat "$FAKE/pushed") size: 2278"
    exit 0 ;;
  image)
    case "$2" in
      inspect)
        if [[ "$*" == *RepoDigests* ]]; then
          if [ -f "$FAKE/pushed" ]; then echo "${3%%:*}@$(cat "$FAKE/pushed")"
          elif [ -f "$FAKE/carries" ]; then cat "$FAKE/carries"; fi
        else
          echo "   id sha256:0000   1 bytes"
        fi
        exit 0 ;;
      ls) [ ! -f "$FAKE/others" ] || cat "$FAKE/others"; exit 0 ;;
    esac ;;
  run)
    shift
    [ "${1:-}" != "--rm" ] || shift
    mount=""
    if [ "${1:-}" = "-v" ]; then mount="$2"; shift 2; fi
    [ "${1:-}" = "$SKOPEO" ] || { echo "fake docker: not skopeo: ${1:-}" >&2; exit 9; }
    shift
    cmd="$1"; shift
    if [ -f "$FAKE/down" ]; then
      echo 'time="2026-10-08T00:00:00Z" level=fatal msg="pinging container registry registry.gitlab.com: Get \"https://registry.gitlab.com/v2/\": dial tcp: i/o timeout"' >&2
      exit 2
    fi
    case "$cmd" in
      inspect)
        name="${*: -1}"; name="${name#docker://}"
        if [[ "$name" == *@sha256:* ]]; then f=$(by_digest "${name#*@}"); else f="$reg/${name##*:}"; [ -f "$f" ] || f=""; fi
        [ -n "$f" ] || unknown "$name"
        cat "$f"; exit 0 ;;
      copy)
        [ -n "$mount" ] || { echo "fake docker: a copy without the login mounted" >&2; exit 9; }
        [[ "$mount" == *:ro ]] || { echo "fake docker: the login mounted writable: $mount" >&2; exit 9; }
        from="${*: -2:1}"; to="${*: -1}"; from="${from#docker://}"; to="${to#docker://}"
        f=$(by_digest "${from#*@}")
        [ -n "$f" ] || unknown "$from"
        if [ -f "$FAKE/corrupt-copy" ]; then echo '{"altered": true}' > "$reg/${to##*:}"; else cp "$f" "$reg/${to##*:}"; fi
        exit 0 ;;
    esac ;;
esac
echo "fake docker: unexpected call: $*" >&2
exit 9
'''


def run_push(bash, registry_tags, new_manifest, carries=None, others="", down=False, corrupt_copy=False, login=None):
    """build/ci/build-linux-image.sh --push, run with the fake docker and a registry of
    files: registry_tags maps each tag to its manifest's bytes, new_manifest is what the
    push gives the tag, carries the RepoDigests the built image holds, login the docker
    configuration's text (None for the usual one, "" for none). Returns (exit code,
    output, the calls made, {tag: digest} as the registry is left)."""
    base = tempfile.mkdtemp(prefix="hm-ci-image-")
    try:
        fake = os.path.join(base, "fake").replace("\\", "/")
        config = os.path.join(base, "docker").replace("\\", "/")
        for d in (os.path.join(fake, "bin"), os.path.join(fake, "registry"), config):
            os.makedirs(d)
        docker = os.path.join(fake, "bin", "docker")
        with io.open(docker, "w", encoding="utf-8", newline="\n") as f:
            f.write(FAKE_DOCKER)
        # Executable by the user who runs the self-test, which is all the script's PATH
        # lookup needs, and by nobody else: the file keeps the mode it was made with and
        # gains the owner's execute bit alone (inside mkdtemp's directory, which is 0700).
        os.chmod(docker, os.stat(docker).st_mode | stat.S_IXUSR)
        for tag, manifest in registry_tags.items():
            with open(os.path.join(fake, "registry", tag), "wb") as f:
                f.write(manifest)
        with open(os.path.join(fake, "new-manifest"), "wb") as f:
            f.write(new_manifest)
        if carries is not None:
            with io.open(os.path.join(fake, "carries"), "w", encoding="utf-8", newline="\n") as f:
                f.write(carries + "\n")
        with io.open(os.path.join(fake, "others"), "w", encoding="utf-8", newline="\n") as f:
            f.write(others)
        for name, present in (("down", down), ("corrupt-copy", corrupt_copy)):
            if present:
                open(os.path.join(fake, name), "w").close()
        if login is None:
            login = '{\n\t"auths": {\n\t\t"registry.gitlab.com": {\n\t\t\t"auth": "bm90LWEtdG9rZW4="\n\t\t}\n\t}\n}\n'
        if login != "":
            with io.open(os.path.join(config, "config.json"), "w", encoding="utf-8", newline="\n") as f:
                f.write(login)
        env = dict(os.environ)
        env["PATH"] = os.path.join(fake, "bin") + os.pathsep + env.get("PATH", "")
        env["FAKE"] = fake
        env["SKOPEO"] = SKOPEO
        env["DOCKER_CONFIG"] = config
        run = subprocess.run([bash, SCRIPT, "--push"], cwd=ROOT, env=env, capture_output=True, text=True,
                             encoding="utf-8", errors="replace")
        calls = []
        if os.path.isfile(os.path.join(fake, "calls")):
            with io.open(os.path.join(fake, "calls"), "r", encoding="utf-8") as f:
                calls = f.read().splitlines()
        left = {}
        for tag in sorted(os.listdir(os.path.join(fake, "registry"))):
            with open(os.path.join(fake, "registry", tag), "rb") as f:
                left[tag] = "sha256:" + hashlib.sha256(f.read()).hexdigest()
        return run.returncode, run.stdout + run.stderr, calls, left
    finally:
        shutil.rmtree(base, ignore_errors=True)


def self_test():
    failures = []

    def expect(what, condition):
        print("%s  %s" % ("ok  " if condition else "FAIL", what))
        if not condition:
            failures.append(what)

    dockerfile = b"FROM ubuntu:26.04@sha256:" + b"a" * 64 + b"\nRUN true\n"
    tag = expected_tag(dockerfile)
    digest = "0123456789abcdef" * 4
    pinned = "%s:%s@sha256:%s" % (REGISTRY, tag, digest)

    expect("the tag is made from the Dockerfile, whatever its line endings",
           tag == expected_tag(dockerfile.replace(b"\n", b"\r\n")) and re.match(r"^26\.04-[0-9a-f]{12}$", tag or ""))
    expect("a FROM not pinned by digest is refused",
           dockerfile_problems(b"FROM ubuntu:26.04\nRUN true\n") != [] and dockerfile_problems(dockerfile) == [])
    expect("the tag on linix, the Dockerfile's: passed", ci_problems("    name: %s:%s\n" % (LOCAL, tag), tag) == [])
    expect("the tag on linix, an older Dockerfile's: refused",
           ci_problems("    name: %s:26.04-000000000000\n" % LOCAL, tag) != [])
    expect("the registry by digest, the Dockerfile's tag: passed", ci_problems("    name: %s\n" % pinned, tag) == [])
    expect("the registry by tag alone: refused", ci_problems("    name: %s:%s\n" % (REGISTRY, tag), tag) != [])
    expect("the registry by a digest cut short: refused",
           ci_problems("    name: %s:%s@sha256:%s\n" % (REGISTRY, tag, digest[:40]), tag) != [])
    expect("the registry by digest, an older Dockerfile's tag: refused",
           ci_problems("    name: %s:26.04-000000000000@sha256:%s\n" % (REGISTRY, digest), tag) != [])
    expect("the registry by two digests: refused",
           ci_problems("    name: %s\n    name: %s:%s@sha256:%s\n" % (pinned, REGISTRY, tag, "f" * 64), tag) != [])
    expect("both names at once: refused",
           ci_problems("    name: %s:%s\n    name: %s\n" % (LOCAL, tag, pinned), tag) != [])
    expect("neither: refused", ci_problems("image: ubuntu:26.04\n", tag) != [])
    expect("a comment naming the registry is no name: the tag on linix still passes",
           ci_problems("# pushed to %s by build-linux-image.sh --push\n    name: %s:%s\n" % (REGISTRY, LOCAL, tag), tag) == [])
    expect("the registry with no tag and no digest: refused", ci_problems("    name: %s\n" % REGISTRY, tag) != [])
    expect("a job in the image by digest with this Dockerfile's tag: passed", job_problems(pinned, tag) == [])
    expect("a job in the image by its tag on linix: refused", job_problems("%s:%s" % (LOCAL, tag), tag) != [])
    expect("a job in the registry image by tag alone: refused", job_problems("%s:%s" % (REGISTRY, tag), tag) != [])
    expect("a job in another registry's image of the same name: refused",
           job_problems("registry.example.test/hmailserver/hmailserver/ci-linux:%s@sha256:%s" % (tag, digest), tag) != [])
    expect("a job in an image built from another Dockerfile: refused",
           job_problems("%s:26.04-000000000000@sha256:%s" % (REGISTRY, digest), tag) != [])
    expect("a job outside GitLab CI: refused", job_problems("", tag) != [])

    # The files .gitlab-ci.yml includes, and the --job step once the image is named by
    # digest: on .linix, and on a job that takes .linix's image without extending it.
    local_linix = ".linix:\n  extends: .own-runners\n  image:\n    name: %s:%s\n    pull_policy: if-not-present\n" % (LOCAL, tag)
    digest_linix = (".linix:\n  extends: .own-runners\n  image:\n    name: %s\n    pull_policy: if-not-present\n"
                    "  before_script:\n    - python3 build/check-ci-image.py --job\n" % pinned)
    borrower = "container-image-sbom:\n  stage: release\n  image: !reference [.linix, image]\n  script:\n    - bash x.sh\n"
    borrower_with_step = borrower.replace("  script:", "  before_script: !reference [.linix, before_script]\n  script:")
    expect("an included file that names the image by an older Dockerfile's tag: refused",
           tree_problems(dockerfile, [(".gitlab-ci.yml", local_linix),
                                      (".gitlab/ci/x.yml", "job:\n  image: %s:26.04-000000000000\n" % LOCAL)]) != [])
    expect("an included file that names the registry image while .linix names the tag on linix: refused",
           tree_problems(dockerfile, [(".gitlab-ci.yml", local_linix), (".gitlab/ci/x.yml", "job:\n  image: %s\n" % pinned)]) != [])
    expect("the tag on linix, a job borrowing .linix's image without a step: passed (the step needs the digest)",
           tree_problems(dockerfile, [(".gitlab-ci.yml", local_linix), (".gitlab/ci/x.yml", borrower)]) == [])
    expect("by digest, .linix's step and a borrower's: passed",
           tree_problems(dockerfile, [(".gitlab-ci.yml", digest_linix), (".gitlab/ci/x.yml", borrower_with_step)]) == [])
    expect("by digest, .linix without the step: refused",
           tree_problems(dockerfile, [(".gitlab-ci.yml", digest_linix.replace("    - python3 build/check-ci-image.py --job\n",
                                                                              "    - true\n"))]) != [])
    expect("by digest, the step only in a comment: refused",
           tree_problems(dockerfile, [(".gitlab-ci.yml", digest_linix.replace("    - python3", "    # - python3"))]) != [])
    expect("by digest, a job borrowing .linix's image without its step: refused",
           tree_problems(dockerfile, [(".gitlab-ci.yml", digest_linix), (".gitlab/ci/x.yml", borrower)]) != [])
    expect("by digest, a borrower's step is its own, not the next job's",
           tree_problems(dockerfile, [(".gitlab-ci.yml", digest_linix),
                                      (".gitlab/ci/x.yml", borrower + "\nnext-job:\n  before_script: !reference [.linix, before_script]\n")]) != [])
    child = "container-image-start-arm64:\n  extends: container-image-sbom\n  before_script:\n    - sh build/ci/arm64-emulation.sh x\n"
    expect("by digest, a job extending a borrower with a before_script of its own that drops the step: refused",
           tree_problems(dockerfile, [(".gitlab-ci.yml", digest_linix),
                                      (".gitlab/ci/x.yml", borrower_with_step + child)]) != [])
    expect("by digest, the same job with the step first in its own before_script: passed",
           tree_problems(dockerfile, [(".gitlab-ci.yml", digest_linix), (".gitlab/ci/x.yml", borrower_with_step + child.replace(
               "  before_script:\n", "  before_script:\n    - !reference [.linix, before_script]\n"))]) == [])
    expect("by digest, a job extending .linix (as a list) and nothing more: passed",
           tree_problems(dockerfile, [(".gitlab-ci.yml", digest_linix + "linux-build:\n  extends:\n    - .linix\n  script:\n    - true\n")]) == [])
    expect("by digest, a job extending .linix with a before_script of its own that drops the step: refused",
           tree_problems(dockerfile, [(".gitlab-ci.yml", digest_linix + "linux-build:\n  extends: .linix\n  before_script:\n    - true\n")]) != [])
    expect("by digest, a job extending .linix that names an image of its own and keeps the step: refused",
           tree_problems(dockerfile, [(".gitlab-ci.yml", digest_linix + "other:\n  extends: [.linix]\n  image: ubuntu:26.04\n")]) != [])

    # The script's text: each step there, in its order.
    with io.open(SCRIPT, "r", encoding="utf-8") as f:
        script = f.read()
    expect("build-linux-image.sh as it is: passed", script_problems(script) == [])
    for needle, why in SCRIPT_STEPS + SCRIPT_LOGIN:
        expect("build-linux-image.sh without %s: refused" % why, script_problems(script.replace(needle, "")) != [])
    read, pushed = 'previous=$(registry_digest "$remote")', 'docker push "$remote"'
    expect("build-linux-image.sh reading the digest after the push: refused",
           script_problems(script.replace(read, "", 1).replace(pushed, pushed + "\n  " + read, 1)) != [])

    # The script run, with the fake docker: the registry's tag names the old manifest,
    # the push gives it the new one, and the old digest is kept under the dated tag.
    bash = bash_program()
    expect("a bash to run build-linux-image.sh with (Git's on Windows)", bash is not None)
    if bash is not None:
        with open(DOCKERFILE, "rb") as f:
            real_tag = expected_tag(f.read())
        old, new, other = b'{"m": "old"}\n', b'{"m": "new"}\n', b'{"m": "other"}\n'
        old_digest = "sha256:" + hashlib.sha256(old).hexdigest()
        new_digest = "sha256:" + hashlib.sha256(new).hexdigest()
        before = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d")
        code, out, calls, left = run_push(bash, {real_tag: old}, new, others="%s:26.04-000000000000\n" % LOCAL)
        after = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d")
        dated = [t for t in left if t in ("%s-%s" % (real_tag, before), "%s-%s" % (real_tag, after))]
        copies = [i for i, c in enumerate(calls) if " copy --quiet --all --preserve-digests " in c]
        pushes = [i for i, c in enumerate(calls) if c.startswith("push ")]
        expect("a rebuild: the script exits 0 (%s)" % out.strip().splitlines()[-1:], code == 0)
        expect("a rebuild: the old digest is kept under <tag>-<yyyymmdd>, the tag names the new one",
               len(dated) == 1 and left[dated[0]] == old_digest and left.get(real_tag) == new_digest)
        expect("a rebuild: one copy, from the old digest to the dated tag, before the one push",
               len(copies) == 1 and len(pushes) == 1 and copies[0] < pushes[0] and dated and
               calls[copies[0]].endswith('--dest-authfile /run/hm-registry-login.json docker://%s@%s docker://%s:%s'
                                         % (REGISTRY, old_digest, REGISTRY, dated[0])))
        expect("a rebuild: the copy's login is the configuration's file, read-only",
               bool(copies) and re.search(r"^run --rm -v \S+/config\.json:/run/hm-registry-login\.json:ro %s copy " % re.escape(SKOPEO),
                                           calls[copies[0]]))
        expect("a rebuild: the pin line and the dated tag with the old digest are printed",
               "    name: %s:%s@%s" % (REGISTRY, real_tag, new_digest) in out and dated and
               "    %s:%s@%s" % (REGISTRY, dated[0], old_digest) in out)
        expect("a rebuild: the other tags on the host are still listed", "%s:26.04-000000000000" % LOCAL in out)

        code, out, calls, left = run_push(bash, {}, new)
        expect("a tag new to the registry: pushed, nothing copied, nothing kept",
               code == 0 and not any(" copy " in c for c in calls) and any(c.startswith("push ") for c in calls)
               and list(left) == [real_tag] and "there is no digest to keep" in out)

        code, out, calls, left = run_push(bash, {real_tag: old, "%s-%s" % (real_tag, before): old}, new)
        expect("the dated tag already names the old digest: pushed, nothing copied, said so",
               code == 0 and not any(" copy " in c for c in calls) and any(c.startswith("push ") for c in calls)
               and "kept already as %s:%s-%s" % (REGISTRY, real_tag, before) in out)

        code, out, calls, left = run_push(bash, {real_tag: old, "%s-%s" % (real_tag, before): other}, new)
        second = "%s-%s-2" % (real_tag, before)
        expect("the dated tag names another digest: the old one is kept as -<yyyymmdd>-2",
               code == 0 and left.get(second) == old_digest and "    %s:%s@%s" % (REGISTRY, second, old_digest) in out)

        code, out, calls, left = run_push(bash, {real_tag: old}, new, carries="%s@%s" % (REGISTRY, old_digest))
        expect("the image carries the digest the tag names: pushed, nothing copied",
               code == 0 and not any(" copy " in c for c in calls) and any(c.startswith("push ") for c in calls)
               and "the tag will not move" in out)

        code, out, calls, left = run_push(bash, {real_tag: old}, new, down=True)
        expect("the registry cannot be read: stopped before the push, the tag still names the old digest",
               code != 0 and not any(c.startswith("push ") for c in calls) and left.get(real_tag) == old_digest
               and "stopping before the push" in out)

        code, out, calls, left = run_push(bash, {real_tag: old}, new, corrupt_copy=True)
        expect("the copy did not preserve the digest: stopped before the push",
               code == 4 and not any(c.startswith("push ") for c in calls) and left.get(real_tag) == old_digest
               and "the digest was not preserved" in out)

        code, out, calls, left = run_push(bash, {real_tag: old}, new, login='{\n\t"auths": {},\n\t"credsStore": "pass"\n}\n')
        expect("a login kept in a credential helper: refused before the build",
               code == 1 and calls == [] and "credential helper" in out)
        code, out, calls, left = run_push(bash, {real_tag: old}, new, login="")
        expect("no login file: refused before the build", code == 1 and calls == [] and "No docker login to read" in out)
        code, out, calls, left = run_push(bash, {real_tag: old}, new, login='{\n\t"auths": {\n\t\t"docker.io": {\n\t\t\t"auth": "eA=="\n\t\t}\n\t}\n}\n')
        expect("a login to another registry alone: refused before the build",
               code == 1 and calls == [] and "no login to registry.gitlab.com" in out)

    print("")
    if failures:
        print("%d self-test case(s) failed" % len(failures))
        return 1
    print("Every self-test case passed.")
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return self_test()

    with open(DOCKERFILE, "rb") as f:
        raw = f.read()
    tag = expected_tag(raw)

    if "--job" in sys.argv[1:]:
        problems = job_problems(os.environ.get("CI_JOB_IMAGE", ""), tag) + dockerfile_problems(raw)
        if problems:
            for p in problems:
                print("FAIL  " + p)
            return 1
        print("OK    this job runs in %s, by digest, built from this commit's build/ci/linux.Dockerfile."
              % os.environ.get("CI_JOB_IMAGE"))
        return 0

    files = ci_files()
    with io.open(SCRIPT, "r", encoding="utf-8") as f:
        script = f.read()
    problems = tree_problems(raw, files) + script_problems(script)

    if problems:
        for p in problems:
            print("FAIL  " + p)
        return 1

    local, remote = named_images("\n".join(text for _, text in files))
    named = local + remote
    print("OK    the CI files (.gitlab-ci.yml and %d under .gitlab/ci) run the Linux jobs on %s, made from"
          " build/ci/linux.Dockerfile as it is%s; build-linux-image.sh --push keeps the digest the tag named before."
          % (len(files) - 1, ", ".join(named), ", each job in it starting with the --job step" if remote else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
