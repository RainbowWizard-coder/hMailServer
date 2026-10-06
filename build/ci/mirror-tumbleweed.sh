#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Copy one openSUSE Tumbleweed snapshot - its linux/amd64 and linux/arm64 images - from
# registry.opensuse.org into the project's own registry, digests preserved, for the pins
# of .gitlab/ci/packages-rpm.yml. Tumbleweed rolls, and openSUSE's registry drops a
# snapshot within weeks; the copy keeps the digests the CI is pinned to.
#
#   build/ci/mirror-tumbleweed.sh <YYYYMMDD>
#
# Needs docker, and a GitLab token that may write the project's registry on standard
# input (never an argument). It prints the two pins to put in packages-rpm.yml.
set -euo pipefail
DATE=${1:?the snapshot date, YYYYMMDD}
SRC=registry.opensuse.org/opensuse/tumbleweed
DST=registry.gitlab.com/hmailserver/hmailserver/mirror/opensuse-tumbleweed
SKOPEO=quay.io/skopeo/stable:v1.20.0
USER_NAME=${HM_REGISTRY_USER:-Progressiverobot}

index=$(docker run --rm "$SKOPEO" inspect --raw "docker://$SRC:latest")
digests=$(printf '%s' "$index" | python3 -c '
import json, sys
d = json.load(sys.stdin)
for m in d["manifests"]:
    a = m["platform"]["architecture"]
    if a in ("amd64", "arm64"):
        print(a, m["digest"])')
[ "$(printf '%s\n' "$digests" | wc -l)" -eq 2 ] || { echo "the index does not list amd64 and arm64"; exit 1; }

IFS= read -r TOKEN
printf '%s\n' "$TOKEN" | docker run -i --rm --entrypoint sh -e DATE="$DATE" -e SRC="$SRC" -e DST="$DST" \
  -e USER_NAME="$USER_NAME" -e DIGESTS="$digests" "$SKOPEO" -c '
  set -e
  IFS= read -r T
  printf "%s" "$T" | skopeo login --password-stdin -u "$USER_NAME" registry.gitlab.com > /dev/null
  unset T
  printf "%s\n" "$DIGESTS" | while read -r arch digest; do
    skopeo copy --quiet --preserve-digests "docker://$SRC@$digest" "docker://$DST:$DATE-$arch"
    echo "    name: $DST:$DATE-$arch@$digest"
  done
  skopeo logout registry.gitlab.com > /dev/null'
unset TOKEN
