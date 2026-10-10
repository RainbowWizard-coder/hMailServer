#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Copy GitLab's secrets analyzer image, as one digest of registry.gitlab.com's
# security-products/secrets holds it, into the project's own registry, digest preserved:
# the multi-platform index, its linux/amd64 and linux/arm64 images and their attestation
# manifests, byte for byte (skopeo copy --all --preserve-digests). secret_detection
# (.gitlab/ci/gitlab-security.yml) runs the copy, and build/scan-secrets.py, the secret
# scan of the git hooks and the landing, takes its gitleaks from the image that job
# names. GitLab's registry does not keep serving a digest its tag has moved on from: the
# digest pinned on 5 October 2026 answered "manifest unknown" on 7 October, and
# secret_detection failed with runner_configuration_error in master's pipelines of 7 and
# 9 October; the landing's scan could still run only where an earlier pull had left the
# image cached. On 9 October the tag named an image built that morning, the analyzer's
# version (7.40.0), its source revision, gitleaks and the rules all unchanged. A
# blocking job must not stop because GitLab rebuilt an image.
#
#   bash build/ci/mirror-secrets.sh <tag> [sha256:<digest>]
#
# <tag> is the analyzer's version as GitLab tags it (7.40.0, say); without a digest the
# one the tag names now is taken. The copy is tagged <tag>-<the digest's first twelve
# hex digits>, so that a later rebuild under the same version is a tag of its own and
# never moves the one a commit is pinned to (the registry's cleanup policy keeps
# ^v?[0-9]+\.[0-9]+\.[0-9]+.*$ by name). It prints the line to put in
# gitlab-security.yml, which names the copy by that tag and the same digest.
#
# Needs docker, and a docker login to registry.gitlab.com made before it runs, by a
# person whose token may write the project's registry (write_registry). This script
# never asks for a token, never reads one and never prints one, as
# build/ci/mirror-semgrep.sh does not: it hands the file the login wrote -
# $DOCKER_CONFIG/config.json, or ~/.docker/config.json when DOCKER_CONFIG is not set -
# to skopeo, mounted read-only, for the copy's destination alone (--dest-authfile), and
# reads GitLab's registry without credentials. skopeo runs in a container, so the login
# must keep its credential in that file rather than in a credential helper, which
# docker uses when the file names one or a docker-credential-* program is on its PATH;
# the script refuses a file that names one. Best in a directory of its own, removed
# after:
#
#   export DOCKER_CONFIG="$(mktemp -d)"
#   docker login registry.gitlab.com        (it asks for the user name and the token)
#   bash build/ci/mirror-secrets.sh 7.40.0 sha256:<digest>
#   docker logout registry.gitlab.com
#   rm -rf "$DOCKER_CONFIG"; unset DOCKER_CONFIG
#
# Run by a maintainer, by hand, on Linux; nothing in CI pushes it.
#
# A new analyzer can be a new gitleaks, and build/scan-secrets.py reads gitleaks' own
# words: a scan whose git log failed is told from a clean one by the "ERR" lines
# gitleaks 8.30.1 logs (findings_of, and the log its self-test holds). Before the pin in
# gitlab-security.yml moves to an image whose SCANNER_VERSION is another, run
# scan-secrets.py over a range whose base the repository does not have and read what the
# new gitleaks logs; and scan the whole history with both images (the analyzer with
# SECRET_DETECTION_HISTORIC_SCAN=true) and compare the two reports, as on 9 October
# 2026 (docs/ContinuousIntegration.md, "Re-pinning secret_detection's analyzer").
# New rules may change GitLab's allowlist, which build/ci/secret-detection-rules.sh
# refuses, and both scans with it, until its GITLAB_TABLE and the entries it changes are
# brought up to date; and any new version fails build/check-gitleaksignore.py until its
# patterns are read again from the new /gitleaks.toml and its RULES_FROM is set.
#
# What is copied, and that it may be: the analyzer (gitlab-org/security-products/
# analyzers/secrets) is MIT-licensed by GitLab Inc.; GitLab's rules in it
# (secret-detection-rules, /gitleaks.toml) are MIT Expat; gitleaks, which it runs, is
# MIT-licensed; and the image's base is Alpine's minimal root filesystem, whose packages
# keep their own licences and their sources at Alpine's. The copy is that image
# unmodified, with every file and notice in it, for this project's CI only, which each
# of those licences permits.
set -euo pipefail
TAG=${1:?the analyzer version as GitLab tags it, such as 7.40.0}
WANTED=${2:-}
SRC=registry.gitlab.com/security-products/secrets
DST=registry.gitlab.com/hmailserver/hmailserver/mirror/secrets
SKOPEO=quay.io/skopeo/stable:v1.20.0
# The login's file, and where skopeo's container sees it.
LOGIN="${DOCKER_CONFIG:-$HOME/.docker}/config.json"
MOUNTED=/run/hm-registry-login.json

case "$TAG" in
  *[!0-9.]*|.*|*.) echo "the tag is a version such as 7.40.0, not '$TAG'"; exit 1 ;;
esac
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

# The index's digest is the sha256 of its bytes as the registry serves them: read raw,
# kept in a file (a shell variable would drop a trailing newline), and hashed here.
raw=$(mktemp)
trap 'rm -f "$raw"' EXIT
docker run --rm "$SKOPEO" inspect --raw "docker://$SRC:$TAG" > "$raw"
now="sha256:$(sha256sum "$raw" | cut -d' ' -f1)"
DIGEST=${WANTED:-$now}
case "$DIGEST" in
  sha256:[0-9a-f]*) [ ${#DIGEST} -eq 71 ] || { echo "not a sha256 digest: $DIGEST"; exit 1; } ;;
  *) echo "not a sha256 digest: $DIGEST"; exit 1 ;;
esac
if [ "$DIGEST" != "$now" ]; then
  echo "note: $SRC:$TAG names $now now; copying $DIGEST, as asked, if the registry still serves it"
fi
SHORT=${DIGEST#sha256:}
DTAG="$TAG-${SHORT:0:12}"

docker run --rm -v "$LOGIN:$MOUNTED:ro" "$SKOPEO" copy --quiet --all --preserve-digests \
  --dest-authfile "$MOUNTED" "docker://$SRC@$DIGEST" "docker://$DST:$DTAG"

# Read back, without credentials, as a runner pulls it: the copy's tag must name
# the same bytes.
docker run --rm "$SKOPEO" inspect --raw "docker://$DST:$DTAG" > "$raw"
copied="sha256:$(sha256sum "$raw" | cut -d' ' -f1)"
if [ "$copied" != "$DIGEST" ]; then
  echo "the copy's tag names $copied, not $DIGEST: the digest was not preserved"
  exit 1
fi
echo "  image: $DST:$DTAG@$DIGEST"
