#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# A release's OLM bundle of the operator, as OperatorHub.io's repository
# (k8s-operatorhub/community-operators) takes one: operators/hmailserver-operator/
# <version>/ with manifests/, metadata/ and tests/scorecard/, written from the tree's
# platform/packaging/helm/operator/bundle, which spells no release out. Plain POSIX sh
# with busybox's tools, so that it runs in cosign's image after the signature
# (build/ci/operator-image-sign.sh) as well as on a workstation:
#
#   sh build/ci/operator-bundle.sh VERSION IMAGE OUT
#
# VERSION  the release's, which becomes the ClusterServiceVersion's name
#          (hmailserver-operator.vVERSION) and spec.version
# IMAGE    the operator image by its digest, <name>@sha256:<64 hex digits>: the
#          index operator-image-push pushed, which the CSV's containerImage, its
#          deployment and its relatedImages then name, so that what OperatorHub.io
#          installs is the image this pipeline signed and nothing a tag moves to
# OUT      a directory this makes, which must not exist yet; the bundle is
#          OUT/hmailserver-operator/VERSION
#
# Each stamp replaces exactly one line, or the tree's CSV is not the one this was
# written for; the date is the time of the run, and the icon is the fork's,
# platform/packaging/hmailserver.png, inlined as OperatorHub.io requires.
set -eu

bundle=hmailserver/source/Server/platform/packaging/helm/operator/bundle
icon=hmailserver/source/Server/platform/packaging/hmailserver.png
package=hmailserver-operator
csv_file=manifests/$package.clusterserviceversion.yaml
placeholder_image=registry.gitlab.com/hmailserver/hmailserver/operator:latest

fail() {
  echo "OPERATOR-BUNDLE FAILED: $*"
  exit 1
}

[ $# = 3 ] || fail "usage: sh build/ci/operator-bundle.sh VERSION IMAGE OUT"
version="$1"
image="$2"
out="$3"
printf '%s\n' "$version" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z][0-9A-Za-z.-]*)?' \
  || fail "$version is not a SemVer 2 version, which a ClusterServiceVersion's spec.version must be"
printf '%s\n' "$image" | grep -Eqx '[a-z0-9][a-z0-9.:_/-]*@sha256:[0-9a-f]{64}' \
  || fail "$image is not an image by its digest (<name>@sha256:<digest>)"
[ -f "$bundle/$csv_file" ] || fail "$bundle/$csv_file is not here: run this from the root of the checkout"
[ -s "$icon" ] || fail "$icon, the fork's icon, is not here"
[ ! -e "$out" ] || fail "$out exists already; this makes it afresh"

dir="$out/$package/$version"
mkdir -p "$dir"
cp -R "$bundle/manifests" "$bundle/metadata" "$bundle/tests" "$dir/"
csv="$dir/$csv_file"

# once PATTERN WHAT: PATTERN matches exactly one line of the CSV.
once() {
  [ "$(grep -c -- "$1" "$csv")" = 1 ] || fail "the CSV has no one line $2 to stamp ($(grep -c -- "$1" "$csv") found)"
}
once "^  name: $package\\.v0\\.1\\.0\$" "metadata.name"
once '^  version: 0\.1\.0$' "spec.version"
once "^    containerImage: $placeholder_image\$" "containerImage"
once "^ *image: $placeholder_image\$" "deployment image"
once '^    createdAt: "2026-01-01T00:00:00Z"$' "createdAt"
once '^    - base64data: ""$' "icon"
! grep -q '^  relatedImages:' "$csv" || fail "the tree's CSV names relatedImages already; this writes them"

created=$(date -u +%Y-%m-%dT%H:%M:%SZ)
data=$(base64 "$icon" | tr -d '\n')
[ -n "$data" ] || fail "$icon could not be read"
sed -i \
  -e "s|^  name: $package\\.v0\\.1\\.0\$|  name: $package.v$version|" \
  -e "s|^  version: 0\\.1\\.0\$|  version: $version|" \
  -e "s|^    containerImage: $placeholder_image\$|    containerImage: $image|" \
  -e "s|^\\( *\\)image: $placeholder_image\$|\\1image: $image|" \
  -e "s|^    createdAt: \"2026-01-01T00:00:00Z\"\$|    createdAt: \"$created\"|" \
  -e "s|^    - base64data: \"\"\$|    - base64data: \"$data\"|" \
  "$csv"
printf '  relatedImages:\n    - name: %s\n      image: %s\n' "$package" "$image" >> "$csv"

# Read back: each stamp where it was, and no placeholder left.
grep -qx "  name: $package.v$version" "$csv" || fail "the CSV's name was not stamped"
grep -qx "  version: $version" "$csv" || fail "the CSV's version was not stamped"
grep -qx "    containerImage: $image" "$csv" || fail "the CSV's containerImage is not $image"
[ "$(grep -c -- "^ *image: $image\$" "$csv")" = 2 ] || fail "the CSV does not name $image as its deployment's image and its related image"
! grep -q -- "$placeholder_image\|2026-01-01T00:00:00Z\|base64data: \"\"\|$package\\.v0\\.1\\.0" "$csv" || fail "a placeholder is left in the CSV"
grep -q '^  name: hmailserver-operator\.v' "$csv" || fail "the CSV has no name"
grep -qx "  operators.operatorframework.io.bundle.package.v1: $package" "$dir/metadata/annotations.yaml" \
  || fail "the bundle's annotations do not name the package $package"

( cd "$dir" && find . -type f | LC_ALL=C sort | while IFS= read -r f; do sha256sum "$f"; done ) > "$out/SHA256SUMS.txt"
cat "$out/SHA256SUMS.txt"
echo "BUNDLE: $dir, $package.v$version, the operator $image"
