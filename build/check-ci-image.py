#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Linux CI jobs run on the image their Dockerfile describes, and no older one.

The two Linux jobs of .gitlab-ci.yml (linux-build, linux-suite) run on the project's
own runner, linix, in an image built there by hand from build/ci/linux.Dockerfile
(build/ci/build-linux-image.sh) and found by its tag, never pulled. So an edit to
the Dockerfile changes nothing a job runs on until somebody builds it - and a job
that went on naming the old tag would pass or fail on the old image and say
nothing about it.

The tag is therefore made from the file: hmailserver-ci/linux:<release>-<hash>,
where <release> is the Ubuntu release of the FROM line and <hash> the first twelve
hex digits of the SHA-256 of the Dockerfile with its line endings made LF (the
tree checks some files out CRLF; this one is LF by .gitattributes, and the hash
must not depend on either). This checker fails when:

  * .gitlab-ci.yml names no hmailserver-ci/linux image, or names one that is not
    the tag of the Dockerfile as it is now - the Dockerfile was edited and the tag
    was not; the message says the tag to write and the command that builds it;
  * a FROM line of the Dockerfile is not pinned by digest, which would let the
    same tag stand for two different images on two days.

A job whose tag has not been built on linix fails at its first step, naming the
image it could not find: loud, and in the right place.

Usage:  python build/check-ci-image.py
"""

import hashlib
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCKERFILE = os.path.join(ROOT, "build", "ci", "linux.Dockerfile")
CI_FILE = os.path.join(ROOT, ".gitlab-ci.yml")

IMAGE = re.compile(r"hmailserver-ci/linux:[A-Za-z0-9_.\-]+")
FROM = re.compile(r"^FROM\s+(\S+)", re.MULTILINE)
UBUNTU = re.compile(r"^FROM\s+ubuntu:(\d+\.\d+)@sha256:[0-9a-f]{64}\b", re.MULTILINE)


def expected_tag(raw):
    """The tag the Dockerfile's bytes make, or None when it has no pinned Ubuntu FROM."""
    text = raw.replace(b"\r\n", b"\n")
    release = UBUNTU.search(text.decode("utf-8"))
    if not release:
        return None
    digest = hashlib.sha256(text).hexdigest()[:12]
    return "hmailserver-ci/linux:%s-%s" % (release.group(1), digest)


def main():
    problems = []

    with open(DOCKERFILE, "rb") as f:
        raw = f.read()
    text = raw.decode("utf-8").replace("\r\n", "\n")

    for image in FROM.findall(text):
        if "@sha256:" not in image:
            problems.append("build/ci/linux.Dockerfile: FROM %s is not pinned by digest (image@sha256:...)." % image)

    tag = expected_tag(raw)
    if tag is None:
        problems.append("build/ci/linux.Dockerfile: no 'FROM ubuntu:<release>@sha256:<digest>' line to take the release from.")

    with io.open(CI_FILE, "r", encoding="utf-8") as f:
        named = sorted(set(IMAGE.findall(f.read())))

    if not named:
        problems.append(".gitlab-ci.yml names no hmailserver-ci/linux image.")
    elif tag is not None:
        for image in named:
            if image != tag:
                problems.append(
                    ".gitlab-ci.yml names %s, but build/ci/linux.Dockerfile as it is now makes %s.\n"
                    "      Write %s in .gitlab-ci.yml, and build it on linix before the pipeline runs:\n"
                    "      bash build/ci/build-linux-image.sh" % (image, tag, tag))

    if problems:
        for p in problems:
            print("FAIL  " + p)
        return 1

    print("OK    .gitlab-ci.yml runs the Linux jobs on %s, the tag of build/ci/linux.Dockerfile as it is." % tag)
    return 0


if __name__ == "__main__":
    sys.exit(main())
