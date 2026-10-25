#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The operator-image job of .gitlab/ci/operator.yml: the operator's image
# (platform/packaging/helm/operator/Dockerfile, the Helm operator with the project's
# chart) built with kaniko for linux/amd64 and linux/arm64, each kept as a tarball for
# operator-image-push, not pushed. Run in kaniko's debug image (busybox sh), from the
# root of the checkout:
#
#   sh build/ci/operator-image.sh
#
# In:   hmailserver-chart-<version>.tgz, the packaged chart the PUBLISHED release
#       carries (helm-chart-package made it, release-sign signed it, release-publish
#       uploaded it), which the copied chart is held to (build/ci/operator-context.sh).
# Out:  operator-image/operator-<arch>.tar, each platform's image, and SHA256SUMS.txt.
#
# Why from the release and not from helm-chart-package's artifact: this follows the
# release's own image jobs, which a maintainer may start weeks after the tag
# (build/ci/container-image-deb.sh says why), and by then that artifact, kept 30 days,
# may have expired; what the release publishes does not. The transport is not what is
# trusted: busybox's wget, the one kaniko's debug image has, does not check a server's
# TLS certificate, so nothing is sent that a listener could use - no token: the project
# is public and its release answers anonymously. Nor need it be: the image carries the
# tree's own chart, copied from this checkout, and the package is only what that copy
# is held to, file by file, so an altered package can stop the job and put nothing
# into the image.
#
# The Dockerfile has no RUN, so kaniko builds the linux/arm64 image on any machine: it
# takes the base image's linux/arm64 layers and adds the two copies, and runs nothing
# of that platform's. The build is --reproducible (no timestamps), so the same tree and
# base image give the same image. The two labels that change with the commit are given
# here, from it, as build/ci/linux-image.sh gives the server's image them - the
# revision, CI_COMMIT_SHA, and the time, CI_COMMIT_TIMESTAMP - since the base image's
# own would otherwise stand beside the project's source label: Red Hat UBI's vcs-ref
# and build date. Both are fixed by the commit, so the build stays reproducible. Like
# the release's own image, nothing is built while the project's CI/CD variable
# HM_PUBLISH_IMAGE is anything but 1 (hmailserver/docs/ContainerImage.md, "Publishing").
set -eu

out=operator-image
platforms="amd64 arm64"
version_h=hmailserver/source/Server/Common/Application/Version.h
# The project whose releases a server takes, by its id.
project_id=86730559

fail() {
  echo "OPERATOR-IMAGE FAILED: $*"
  exit 1
}

# fetch URL FILE: three tries, anonymously.
fetch() {
  attempt=1
  while :; do
    rm -f "$2"
    if wget -q -U hmailserver-operator-image -O "$2" "$1" 2> "$out/wget.log"; then
      return 0
    fi
    if [ "$attempt" -ge 3 ]; then
      cat "$out/wget.log"
      return 1
    fi
    attempt=$((attempt + 1))
    sleep 5
  done
}

if [ "${HM_PUBLISH_IMAGE:-}" != 1 ]; then
  echo "REFUSED: the project's CI/CD variable HM_PUBLISH_IMAGE is not 1, so no operator image is built."
  exit 3
fi
tag="${CI_COMMIT_TAG:-}"
case "$tag" in
  v[0-9]*) ;;
  *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
esac
version="${tag#v}"
tree_version=$(sed -n 's/^#define[[:space:]][[:space:]]*HMAILSERVER_VERSION[[:space:]][[:space:]]*"\([0-9][0-9.]*\)".*/\1/p' "$version_h" | head -n 1 | tr -d '\r')
printf '%s\n' "$tree_version" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+' || fail "Version.h's HMAILSERVER_VERSION could not be read ('$tree_version')"
[ "${version%%[!0-9.]*}" = "$tree_version" ] || fail "the tag is $tag and Version.h says $tree_version"
# A version is what a package name may hold, and nothing a URL would have to escape.
printf '%s\n' "$version" | grep -Eqx '[0-9][A-Za-z0-9._-]{0,62}' || fail "$version is not a release version"
revision="${CI_COMMIT_SHA:-}"
created="${CI_COMMIT_TIMESTAMP:-}"
printf '%s\n' "$revision" | grep -Eqx '[0-9a-f]{40}' || fail "CI_COMMIT_SHA is '$revision', not a commit"
printf '%s\n' "$created" | grep -Eqx '[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(Z|[+-][0-9]{2}:[0-9]{2})' \
  || fail "CI_COMMIT_TIMESTAMP is '$created', not an ISO 8601 time"
[ "${CI_PROJECT_ID:-}" = "$project_id" ] || fail "the project's id is '${CI_PROJECT_ID:-}', not $project_id, whose releases a server takes"
[ -x /kaniko/executor ] || fail "there is no /kaniko/executor: the job runs in kaniko's debug image"

rm -rf "$out"
mkdir -p "$out/chart"
# The release, which must be published and link the chart where the release chain put
# it (build/ci/release-chain.py, registry_url).
api="https://gitlab.com/api/v4/projects/$project_id"
name="hmailserver-chart-$version.tgz"
url="$api/packages/generic/hmailserver/$version/$name"
fetch "$api/releases/$tag" "$out/release.json" || fail "the release $tag could not be read from $api/releases/$tag: publish it first"
grep -qF "\"tag_name\":\"$tag\"" "$out/release.json" || fail "$api/releases/$tag is not the release $tag"
grep -qF "\"url\":\"$url\"" "$out/release.json" || fail "the release $tag has no link to $url: it does not carry the chart"
package="$out/chart/$name"
fetch "$url" "$package" || fail "$name could not be downloaded from $url"
[ -s "$package" ] || fail "$name is empty"
echo "the release's chart: $name, $(sha256sum "$package" | cut -d' ' -f1)"
rm -f "$out/release.json" "$out/wget.log"

context="$(pwd)/$out/context"
sh build/ci/operator-context.sh "$context" "$version" "$package"

for arch in $platforms; do
  echo ""
  echo "== linux/$arch"
  # --cleanup empties kaniko's filesystem after each build, so that the second starts
  # from nothing of the first's.
  /kaniko/executor --context "dir://$context" --dockerfile "$context/Dockerfile" \
    --custom-platform "linux/$arch" --build-arg "HM_OPERATOR_VERSION=$version" \
    --label "org.opencontainers.image.revision=$revision" \
    --label "org.opencontainers.image.created=$created" \
    --reproducible --cleanup --no-push --tar-path "$out/operator-$arch.tar" \
    --destination "operator:$version-$arch" \
    || fail "kaniko could not build the linux/$arch image"
  [ -s "$out/operator-$arch.tar" ] || fail "kaniko wrote no $out/operator-$arch.tar"
done
rm -rf "$context" "$out/chart"
( cd "$out" && sha256sum operator-*.tar > SHA256SUMS.txt && cat SHA256SUMS.txt )
echo "BUILT: the operator $version for linux/amd64 and linux/arm64, in $out/, not pushed."
