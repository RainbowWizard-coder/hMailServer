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
# In:   helm-chart/hmailserver-chart-<version>.tgz and its SHA256SUMS.txt, the release's
#       packaged chart (helm-chart-package's artifact), which the copied chart is held to
#       (build/ci/operator-context.sh).
# Out:  operator-image/operator-<arch>.tar, each platform's image, and SHA256SUMS.txt.
#
# The Dockerfile has no RUN, so kaniko builds the linux/arm64 image on any machine: it
# takes the base image's linux/arm64 layers and adds the two copies, and runs nothing
# of that platform's. The build is --reproducible (no timestamps), so the same tree and
# base image give the same image. Like the release's own image, nothing is built while
# the project's CI/CD variable HM_PUBLISH_IMAGE is anything but 1
# (hmailserver/docs/ContainerImage.md, "Publishing").
set -eu

out=operator-image
platforms="amd64 arm64"
version_h=hmailserver/source/Server/Common/Application/Version.h

fail() {
  echo "OPERATOR-IMAGE FAILED: $*"
  exit 1
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
[ -x /kaniko/executor ] || fail "there is no /kaniko/executor: the job runs in kaniko's debug image"

package="helm-chart/hmailserver-chart-$version.tgz"
[ -f helm-chart/SHA256SUMS.txt ] || fail "helm-chart/SHA256SUMS.txt is not here: helm-chart-package's artifact did not arrive"
( cd helm-chart && sha256sum -c SHA256SUMS.txt ) || fail "$package is not what helm-chart-package packaged"
grep -q " hmailserver-chart-$version.tgz\$" helm-chart/SHA256SUMS.txt || fail "helm-chart/SHA256SUMS.txt does not name hmailserver-chart-$version.tgz"

rm -rf "$out"
mkdir -p "$out"
context="$(pwd)/$out/context"
sh build/ci/operator-context.sh "$context" "$version" "$package"

for arch in $platforms; do
  echo ""
  echo "== linux/$arch"
  # --cleanup empties kaniko's filesystem after each build, so that the second starts
  # from nothing of the first's.
  /kaniko/executor --context "dir://$context" --dockerfile "$context/Dockerfile" \
    --custom-platform "linux/$arch" --build-arg "HM_OPERATOR_VERSION=$version" \
    --reproducible --cleanup --no-push --tar-path "$out/operator-$arch.tar" \
    --destination "operator:$version-$arch" \
    || fail "kaniko could not build the linux/$arch image"
  [ -s "$out/operator-$arch.tar" ] || fail "kaniko wrote no $out/operator-$arch.tar"
done
rm -rf "$context"
( cd "$out" && sha256sum operator-*.tar > SHA256SUMS.txt && cat SHA256SUMS.txt )
echo "BUILT: the operator $version for linux/amd64 and linux/arm64, in $out/, not pushed."
