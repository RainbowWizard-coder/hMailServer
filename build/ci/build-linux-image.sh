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
#   bash build/ci/build-linux-image.sh --push     builds it, keeps the digest the
#                                                 registry's tag names now under a
#                                                 dated tag, pushes it to the project's
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
# The digest the tag named before is kept. A rebuild from an unchanged Dockerfile -
# for Ubuntu's updates - pushes a new manifest under the same tag, and the registry's
# tag moves to it; the digest it named before, which .linix at master and at every
# open batch pins until a commit of their own moves the pin, would be left with no
# tag, and GitLab.com's registry removes an untagged manifest that nothing references
# about 24 hours later (its online garbage collection; the cleanup policy, a rule
# over tags, neither causes nor prevents it). So before the push, --push reads the
# digest the registry's tag names now - without credentials, as a runner pulls it:
# skopeo inspect --raw in a container, the bytes hashed here - and, unless the image
# about to be pushed carries that digest already (the tag would not move), gives it
# the dated tag <tag>-<yyyymmdd>, the UTC date of the day, by a copy inside the
# registry that preserves the digest (skopeo copy --all --preserve-digests, which
# refuses to alter the manifest), reads the dated tag back and holds it to the
# digest, and only then pushes. The push's report names both the new pin line and
# the dated tag that keeps the previous one. A second rebuild the same day finds the
# dated tag taken: when it names the digest to keep, nothing more is done; otherwise
# the first free of -<yyyymmdd>-2, -3, ... is used. A tag new to the registry (a
# changed Dockerfile) has nothing to keep. When the registry cannot be read, the
# script stops before the push rather than move the tag off a digest it could not
# keep; and when the copy's read-back is not the digest, it stops too. The registry's
# cleanup policy keeps the dated tags by name (^[0-9]{2}\.[0-9]{2}-[0-9a-f]{12}-[0-9]{8}$;
# a -2 is outside that pattern, and the policy is switched off). On 8 October 2026
# the 6 October digest was kept so by hand, as 26.04-52e9976e73bf-20261006, a day
# after the rebuild moved the tag off it; build/check-ci-image.py holds this script
# to the steps, and its self-test runs them against a docker of its own.
#
# For the copy, the script hands the file the docker login wrote -
# $DOCKER_CONFIG/config.json, or ~/.docker/config.json when DOCKER_CONFIG is not set -
# to skopeo's container, mounted read-only, for the copy's destination alone
# (--dest-authfile), as build/ci/mirror-semgrep.sh does. skopeo runs in a container,
# so the login must keep its credential in that file rather than in a credential
# helper, which docker uses when the file names one or a docker-credential-* program
# is on its PATH; the script refuses a file that names one, before it builds. Best in
# a directory of its own, removed after:
#
#   export DOCKER_CONFIG="$(mktemp -d)"
#   docker login registry.gitlab.com        (it asks for the user name and the token)
#   bash build/ci/build-linux-image.sh --push
#   docker logout registry.gitlab.com
#   rm -rf "$DOCKER_CONFIG"; unset DOCKER_CONFIG
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
skopeo="quay.io/skopeo/stable:v1.20.0"
# The login's file, and where skopeo's container sees it.
login="${DOCKER_CONFIG:-$HOME/.docker}/config.json"
mounted=/run/hm-registry-login.json

here="$(cd "$(dirname "$0")" && pwd)"
dockerfile="$here/linux.Dockerfile"

release=$(sed -n 's/^FROM ubuntu:\([0-9][0-9]*\.[0-9][0-9]*\)@.*$/\1/p' "$dockerfile" | head -n 1)
if [ -z "$release" ]; then
  echo "No 'FROM ubuntu:<release>@<digest>' line in $dockerfile." >&2
  exit 2
fi
digest=$(sed 's/\r$//' "$dockerfile" | sha256sum | cut -c1-12)
tag="hmailserver-ci/linux:${release}-${digest}"

# The digest a name in the registry has now - the sha256 of the manifest's bytes as
# the registry serves them, read without credentials as a runner pulls them, kept in
# a file (a shell variable would drop a trailing newline) and hashed here, as
# mirror-semgrep.sh reads one - or nothing when the registry holds no manifest by
# that name. Any other failure to read stops the script: a push that moved the tag
# off a digest this could not read would lose it.
registry_digest() {
  local raw err
  raw=$(mktemp)
  err=$(mktemp)
  if docker run --rm "$skopeo" inspect --raw "docker://$1" > "$raw" 2> "$err"; then
    printf 'sha256:%s' "$(sha256sum "$raw" | cut -d' ' -f1)"
  elif grep -Eq 'manifest unknown|name unknown' "$err"; then
    :
  else
    cat "$err" >&2
    echo "Could not read what $1 names in the registry; stopping before the push moves the tag." >&2
    rm -f "$raw" "$err"
    return 1
  fi
  rm -f "$raw" "$err"
}

if [ "$push" = 1 ]; then
  # Before the build, which takes minutes: the login the copy hands to skopeo.
  if [ ! -f "$login" ]; then
    echo "No docker login to read: $login is not there (log in first, as this script's header says)." >&2
    exit 1
  fi
  if grep -Eq '"(credsStore|credHelpers)"' "$login"; then
    echo "$login keeps its logins in a credential helper, which skopeo cannot reach from its container:" >&2
    echo "log in with a DOCKER_CONFIG of its own, where docker finds no helper (this script's header)." >&2
    exit 1
  fi
  if ! grep -Fq '"registry.gitlab.com"' "$login"; then
    echo "$login holds no login to registry.gitlab.com (log in first, as this script's header says)." >&2
    exit 1
  fi
fi

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

  # What the tag names now, kept under a dated tag before the push moves it.
  echo ""
  echo "== reading what $remote names now"
  previous=$(registry_digest "$remote")
  kept=""
  if [ -z "$previous" ]; then
    echo "   nothing: the tag is new to the registry, and there is no digest to keep"
  elif docker image inspect "$tag" --format '{{range .RepoDigests}}{{println .}}{{end}}' | grep -Fxq "$registry@$previous"; then
    echo "   $previous, which the image about to be pushed is already: the tag will not move"
  else
    today=$(date -u +%Y%m%d)
    kept="${release}-${digest}-${today}"
    n=1
    while :; do
      names=$(registry_digest "$registry:$kept")
      if [ -z "$names" ] || [ "$names" = "$previous" ]; then
        break
      fi
      n=$((n + 1))
      kept="${release}-${digest}-${today}-${n}"
    done
    if [ "$names" = "$previous" ]; then
      echo "   $previous, kept already as $registry:$kept"
    else
      echo "   $previous; keeping it as $registry:$kept before the tag moves"
      docker run --rm -v "$login:$mounted:ro" "$skopeo" copy --quiet --all --preserve-digests \
        --dest-authfile "$mounted" "docker://$registry@$previous" "docker://$registry:$kept"
      # Read back without credentials, as a runner pulls it: the dated tag must name
      # the same bytes.
      copied=$(registry_digest "$registry:$kept")
      if [ "$copied" != "$previous" ]; then
        echo "$registry:$kept names ${copied:-nothing}, not $previous: the digest was not preserved; stopping before the push moves the tag." >&2
        exit 4
      fi
    fi
  fi

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
  if [ -n "$kept" ]; then
    if [ "$pinned" = "$previous" ]; then
      echo "The push moved nothing: $remote names $pinned as before, and so does $registry:$kept."
    else
      echo "The digest the tag named before, which the commits pinned to it pull as before, is kept as"
      echo ""
      echo "    $registry:$kept@$previous"
    fi
    echo ""
  fi
fi

others=$(docker image ls --format '{{.Repository}}:{{.Tag}}' 'hmailserver-ci/linux' | grep -vxF "$tag" || true)
if [ -n "$others" ]; then
  echo ""
  echo "Other tags of the image are still here; remove one with 'docker image rm <tag>' once no"
  echo "pipeline for a commit that names it will run again:"
  printf '   %s\n' $others
fi
