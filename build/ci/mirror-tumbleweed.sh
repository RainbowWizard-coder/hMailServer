#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Copy one openSUSE Tumbleweed snapshot - its linux/amd64 and linux/arm64 images - from
# registry.opensuse.org into the project's own registry, digests preserved, for the pins
# of .gitlab/ci/packages-rpm.yml. Tumbleweed rolls, and openSUSE's registry drops a
# snapshot within weeks; the copy keeps the digests the CI is pinned to.
#
#   bash build/ci/mirror-tumbleweed.sh <YYYYMMDD>
#
# Needs docker, and a docker login to registry.gitlab.com made before it runs, by a
# person whose token may write the project's registry (write_registry). This script
# never asks for a token, never reads one and never prints one, as
# build/ci/build-linux-image.sh does not: it hands the file the login wrote -
# $DOCKER_CONFIG/config.json, or ~/.docker/config.json when DOCKER_CONFIG is not set -
# to skopeo, mounted read-only, for the copy's destination alone (--dest-authfile), and
# reads openSUSE's registry without credentials. skopeo runs in a container, so the
# login must keep its credential in that file rather than in a credential helper,
# which docker uses when the file names one or a docker-credential-* program is on its
# PATH; the script refuses a file that names one. Best in a directory of its own,
# removed after:
#
#   export DOCKER_CONFIG="$(mktemp -d)"
#   docker login registry.gitlab.com        (it asks for the user name and the token)
#   bash build/ci/mirror-tumbleweed.sh <YYYYMMDD>
#   docker logout registry.gitlab.com
#   rm -rf "$DOCKER_CONFIG"; unset DOCKER_CONFIG
#
# Run by a maintainer, by hand, on Linux. It prints the two pins to put in
# packages-rpm.yml.
set -euo pipefail
DATE=${1:?the snapshot date, YYYYMMDD}
SRC=registry.opensuse.org/opensuse/tumbleweed
DST=registry.gitlab.com/hmailserver/hmailserver/mirror/opensuse-tumbleweed
SKOPEO=quay.io/skopeo/stable:v1.20.0
# The login's file, and where skopeo's container sees it.
LOGIN="${DOCKER_CONFIG:-$HOME/.docker}/config.json"
MOUNTED=/run/hm-registry-login.json

if [ ! -f "$LOGIN" ]; then
  echo "no docker login to read: $LOGIN is not there (log in first, as this script's header says)"
  exit 1
fi
if grep -Eq '"(credsStore|credHelpers)"' "$LOGIN"; then
  echo "$LOGIN keeps its logins in a credential helper, which skopeo cannot reach from its container:"
  echo "log in with a DOCKER_CONFIG of its own, where docker finds no helper (this script's header)"
  exit 1
fi
if ! grep -Fq '"registry.gitlab.com"' "$LOGIN"; then
  echo "$LOGIN holds no login to registry.gitlab.com (log in first, as this script's header says)"
  exit 1
fi

index=$(docker run --rm "$SKOPEO" inspect --raw "docker://$SRC:latest")
digests=$(printf '%s' "$index" | python3 -c '
import json, sys
d = json.load(sys.stdin)
for m in d["manifests"]:
    a = m["platform"]["architecture"]
    if a in ("amd64", "arm64"):
        print(a, m["digest"])')
[ "$(printf '%s\n' "$digests" | wc -l)" -eq 2 ] || { echo "the index does not list amd64 and arm64"; exit 1; }

printf '%s\n' "$digests" | while read -r arch digest; do
  docker run --rm -v "$LOGIN:$MOUNTED:ro" "$SKOPEO" copy --quiet --preserve-digests \
    --dest-authfile "$MOUNTED" "docker://$SRC@$digest" "docker://$DST:$DATE-$arch" < /dev/null
  echo "    name: $DST:$DATE-$arch@$digest"
done
