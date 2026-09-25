#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Builds the Linux CI jobs' image (build/ci/linux.Dockerfile) on linix, under the
# tag .gitlab-ci.yml names, and says what it built. Run it by hand, on linix, as a
# member of the docker group, from any checkout of the commit whose Dockerfile is
# wanted:
#
#   bash build/ci/build-linux-image.sh
#
# The tag is hmailserver-ci/linux:<ubuntu release>-<first twelve hex digits of the
# SHA-256 of the Dockerfile with its line endings made LF>, the rule
# build/check-ci-image.py holds .gitlab-ci.yml to. So the tag this prints is the
# one the pipeline asks for at the same commit, and a Dockerfile that has not
# changed rebuilds to the same tag (docker replaces it; the old layers become
# dangling and `docker image prune` removes them).
#
# Why by hand and not a CI job: a job that builds an image needs the host's docker
# socket mounted into the runner's containers, which gives every job on that
# runner root on linix. Whether the runner gets that is the integrator's decision
# (docs/ContinuousIntegration.md, "The image"); until it is taken, this is the way.
#
# It never removes an older tag: a pipeline for an older commit (master, while a
# batch branch carries a new Dockerfile) still asks for it. It lists them, with
# the command that removes one.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
dockerfile="$here/linux.Dockerfile"

release=$(sed -n 's/^FROM ubuntu:\([0-9][0-9]*\.[0-9][0-9]*\)@.*$/\1/p' "$dockerfile" | head -n 1)
if [ -z "$release" ]; then
  echo "No 'FROM ubuntu:<release>@<digest>' line in $dockerfile." >&2
  exit 2
fi
digest=$(sed 's/\r$//' "$dockerfile" | sha256sum | cut -c1-12)
tag="hmailserver-ci/linux:${release}-${digest}"

echo "== building $tag from $dockerfile"
# The context is build/ci alone: the Dockerfile copies nothing from the tree.
docker build --file "$dockerfile" --tag "$tag" \
  --label "org.opencontainers.image.title=hMailServer Linux CI" \
  --label "org.opencontainers.image.description=The image the Linux jobs of .gitlab-ci.yml run in" \
  "$here"

echo ""
echo "== built $tag"
docker image inspect "$tag" --format '   id {{.Id}}   {{.Size}} bytes'

others=$(docker image ls --format '{{.Repository}}:{{.Tag}}' 'hmailserver-ci/linux' | grep -vxF "$tag" || true)
if [ -n "$others" ]; then
  echo ""
  echo "Other tags of the image are still here; remove one with 'docker image rm <tag>' once no"
  echo "pipeline for a commit that names it will run again:"
  printf '   %s\n' $others
fi
