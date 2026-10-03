#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Builds the Linux CI jobs' image (build/ci/linux.Dockerfile) on linix, under the
# tag .gitlab-ci.yml names, and says what it built. Run it by hand, on linix, as a
# member of the docker group, from any checkout of the commit whose Dockerfile is
# wanted:
#
#   bash build/ci/build-linux-image.sh            builds hmailserver-ci/linux:<tag>
#   bash build/ci/build-linux-image.sh --push     builds it, pushes it to the project's
#                                                 container registry and prints the
#                                                 line .gitlab-ci.yml names it by
#
# --push puts the image at registry.gitlab.com/hmailserver/hmailserver/ci-linux:<tag>
# and prints that name with the digest the registry gave its manifest, the form
# build/check-ci-image.py holds .gitlab-ci.yml to once the image is there: Docker takes
# an image named by digest only as the registry holds that digest, so a tag given to
# other bytes on linix - by any job that drives its Docker, as the privileged
# linix-systemd entry's can - is not run in its place, and the release's packages are
# not built in it (docs/ContinuousIntegration.md, "The image"; the second security
# review of 6.3.6, SUP2-1). It needs a docker login to registry.gitlab.com with a
# token that may write the project's registry (write_registry) - a person's, given
# for the push and logged out after it; this script never asks for one and never
# prints one.
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

push=0
case "${1:-}" in
  --push) push=1 ;;
  "") ;;
  *) echo "usage: bash build/ci/build-linux-image.sh [--push]" >&2; exit 2 ;;
esac
registry="registry.gitlab.com/hmailserver/hmailserver/ci-linux"

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

if [ "$push" = 1 ]; then
  remote="$registry:${release}-${digest}"
  echo ""
  echo "== pushing $remote"
  docker tag "$tag" "$remote"
  docker push "$remote"
  # The digest the registry gave the manifest just pushed, as the local store
  # recorded it for that name.
  pinned=$(docker image inspect "$remote" --format '{{range .RepoDigests}}{{println .}}{{end}}' \
             | grep -F "$registry@sha256:" | head -n 1 | sed 's/^.*@//' || true)
  if ! printf '%s' "$pinned" | grep -Eq '^sha256:[0-9a-f]{64}$'; then
    echo "The registry gave $remote no digest docker recorded; nothing to write." >&2
    exit 3
  fi
  echo ""
  echo "== pushed $remote@$pinned"
  echo "In .gitlab-ci.yml, .linix's image (build/check-ci-image.py holds it to this):"
  echo ""
  echo "    name: $remote@$pinned"
  echo ""
fi

others=$(docker image ls --format '{{.Repository}}:{{.Tag}}' 'hmailserver-ci/linux' | grep -vxF "$tag" || true)
if [ -n "$others" ]; then
  echo ""
  echo "Other tags of the image are still here; remove one with 'docker image rm <tag>' once no"
  echo "pipeline for a commit that names it will run again:"
  printf '   %s\n' $others
fi
