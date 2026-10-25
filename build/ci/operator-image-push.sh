#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The operator-image-push job of .gitlab/ci/operator.yml: the operator's two images,
# which operator-image built, joined under one index (a Docker manifest list) and
# pushed to the project's container registry as operator:<version>, beside the
# server's image (registry.gitlab.com/hmailserver/hmailserver/operator). Run in
# regctl's -alpine image (busybox sh), from the root of the checkout:
#
#   sh build/ci/operator-image-push.sh
#
# In:   operator-image/operator-<arch>.tar and SHA256SUMS.txt.
# Out:  operator-image-push/operator.env, the job's dotenv report:
#         HM_OPERATOR_IMAGE         the index, <repository>@sha256:<digest>
#         HM_OPERATOR_IMAGE_AMD64   the linux/amd64 image by its digest
#         HM_OPERATOR_IMAGE_ARM64   the linux/arm64 image by its digest
#       and index.json, the index as pushed.
#
# One tag, the release's version, and no :latest: the bundle names the image by its
# digest (build/ci/operator-bundle.sh), so no tag needs moving. The registry password
# is the job's own CI_REGISTRY_PASSWORD, handed to regctl on its standard input in a
# home folder of the job's, removed at the end; it is never on a command line or
# printed. Nothing is pushed while HM_PUBLISH_IMAGE is anything but 1.
set -eu

out=operator-image-push
platforms="amd64 arm64"

fail() {
  echo "OPERATOR-IMAGE-PUSH FAILED: $*"
  exit 1
}

if [ "${HM_PUBLISH_IMAGE:-}" != 1 ]; then
  echo "REFUSED: the project's CI/CD variable HM_PUBLISH_IMAGE is not 1, so no operator image is pushed."
  exit 3
fi
tag="${CI_COMMIT_TAG:-}"
case "$tag" in
  v[0-9]*) ;;
  *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
esac
version="${tag#v}"
# The commit's revision and time, which operator-image gives each image as its labels
# in place of the base image's (build/ci/operator-image.sh).
revision="${CI_COMMIT_SHA:-}"
created="${CI_COMMIT_TIMESTAMP:-}"
[ -n "$revision" ] && [ -n "$created" ] || fail "the job has no CI_COMMIT_SHA or CI_COMMIT_TIMESTAMP"
case "${CI_REGISTRY_IMAGE:-}" in
  registry.gitlab.com/hmailserver/hmailserver) repository="$CI_REGISTRY_IMAGE/operator" ;;
  *) fail "the project's registry image is '${CI_REGISTRY_IMAGE:-}', not registry.gitlab.com/hmailserver/hmailserver" ;;
esac
[ -n "${CI_REGISTRY:-}" ] && [ -n "${CI_REGISTRY_USER:-}" ] && [ -n "${CI_REGISTRY_PASSWORD:-}" ] \
  || fail "the job has no CI_REGISTRY, CI_REGISTRY_USER or CI_REGISTRY_PASSWORD"
command -v regctl > /dev/null 2>&1 || fail "there is no regctl in this image: the job runs in regclient/regctl's -alpine image"

[ -f operator-image/SHA256SUMS.txt ] || fail "operator-image/SHA256SUMS.txt is not here: operator-image's artifact did not arrive"
( cd operator-image && sha256sum -c SHA256SUMS.txt ) || fail "operator-image's tarballs are not what it built"

rm -rf "$out"
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
HOME="$work/home"
mkdir -p "$HOME"
export HOME
unset REGCTL_CONFIG DOCKER_CONFIG 2> /dev/null || true
layout="ocidir://$work/layout"

for arch in $platforms; do
  tarball="operator-image/operator-$arch.tar"
  [ -s "$tarball" ] || fail "$tarball is not here"
  regctl image import "$layout:$arch" "$tarball" || fail "regctl could not read $tarball"
  regctl image config --format '{{json .}}' "$layout:$arch" > "$work/config-$arch.json" || fail "the linux/$arch image's configuration could not be read"
  grep -q "\"architecture\":\"$arch\"" "$work/config-$arch.json" || fail "$tarball is not an image for $arch"
  grep -q '"os":"linux"' "$work/config-$arch.json" || fail "$tarball is not an image for linux"
  grep -q "\"org.opencontainers.image.version\":\"$version\"" "$work/config-$arch.json" || fail "$tarball is not the operator $version"
  grep -qF "\"org.opencontainers.image.revision\":\"$revision\"" "$work/config-$arch.json" \
    || fail "$tarball's org.opencontainers.image.revision is not the commit $revision"
  grep -qF "\"org.opencontainers.image.created\":\"$created\"" "$work/config-$arch.json" \
    || fail "$tarball's org.opencontainers.image.created is not the commit's time $created"
done

regctl index create "$layout:$version" --media-type application/vnd.docker.distribution.manifest.list.v2+json \
  --ref "$layout:amd64" --ref "$layout:arm64" || fail "regctl could not make the index"
index=$(regctl image digest "$layout:$version") || fail "the index has no digest"
printf '%s\n' "$index" | grep -Eqx 'sha256:[0-9a-f]{64}' || fail "the index's digest is '$index'"
regctl manifest get "$layout:$version" --format raw-body > "$out/index.json" || fail "the index could not be read back"
amd64=$(regctl image digest --platform linux/amd64 "$layout:$version") || fail "the index has no linux/amd64 entry"
arm64=$(regctl image digest --platform linux/arm64 "$layout:$version") || fail "the index has no linux/arm64 entry"
[ "$(grep -o '"digest":"sha256:[0-9a-f]*"' "$out/index.json" | wc -l)" -eq 2 ] || fail "the index names other than the two platforms' images"

printf '%s' "$CI_REGISTRY_PASSWORD" | regctl registry login "$CI_REGISTRY" --user "$CI_REGISTRY_USER" --pass-stdin > /dev/null \
  || fail "regctl could not log in to $CI_REGISTRY with the job's credentials"
regctl image copy "$layout:$version" "$repository:$version" \
  || fail "the index and its two images could not be pushed as $repository:$version; delete that tag if the registry shows it"
pushed=$(regctl image digest "$repository:$version") || fail "$repository:$version could not be read back"
[ "$pushed" = "$index" ] || fail "$repository:$version is $pushed, not the index $index; delete that tag"

{
  echo "HM_OPERATOR_IMAGE=$repository@$index"
  echo "HM_OPERATOR_IMAGE_AMD64=$repository@$amd64"
  echo "HM_OPERATOR_IMAGE_ARM64=$repository@$arm64"
} > "$out/operator.env"
cat "$out/operator.env"
echo "PUSHED: $repository:$version, the index $index of linux/amd64 $amd64 and linux/arm64 $arm64."
