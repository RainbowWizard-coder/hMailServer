#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The helm-chart-package job of .gitlab/ci/helm-chart.yml: the Helm chart
# (hmailserver/source/Server/platform/packaging/helm/hmailserver) packaged as a file
# of the release, at the release's version, with the release's image named in it.
# Run in Alpine's image with Alpine's helm (busybox sh), from the root of the
# checkout:
#
#   sh build/ci/helm-chart-package.sh
#
# Out:  helm-chart/hmailserver-chart-<version>.tgz   the chart, which the release job
#                                                    collects and release-sign signs
#                                                    like every other file
#       helm-chart/SHA256SUMS.txt                    its SHA-256, which collect holds
#                                                    the file to
#
# The version: a v* tag's, which must be Version.h's with or without a suffix, as the
# packages' are; on a batch* branch, where a release is rehearsed, Version.h's. Helm
# takes only a SemVer 2 version, so a pre-release is tagged v<version>-<suffix>
# (v6.3.6-rc1), never v6.3.6rc1, which this refuses. What is stamped, in a copy of the
# chart: version and appVersion (helm package --version --app-version), so that the
# chart installs its own release's image by default; the image Artifact Hub is told
# of (artifacthub.io/images, :latest in the tree); and artifacthub.io/prerelease for
# a version with a suffix. The tree's Chart.yaml carries none of these
# (build/check-linux-version-stamp.sh refuses a version literal under packaging/).
# The packaged chart is then read back - helm show chart, and helm template with the
# values a real installation needs - and held to what was stamped.
set -eu

out=helm-chart
chart=hmailserver/source/Server/platform/packaging/helm/hmailserver
version_h=hmailserver/source/Server/Common/Application/Version.h
image=registry.gitlab.com/hmailserver/hmailserver

fail() {
  echo "HELM-CHART-PACKAGE FAILED: $*"
  exit 1
}

command -v helm > /dev/null 2>&1 || fail "there is no helm here (Alpine: apk add helm)"
helm version --short

tree_version=$(sed -n 's/^#define[[:space:]][[:space:]]*HMAILSERVER_VERSION[[:space:]][[:space:]]*"\([0-9][0-9.]*\)".*/\1/p' "$version_h" | head -n 1 | tr -d '\r')
printf '%s\n' "$tree_version" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+' || fail "Version.h's HMAILSERVER_VERSION could not be read ('$tree_version')"
tag="${CI_COMMIT_TAG:-}"
if [ -n "$tag" ]; then
  case "$tag" in
    v[0-9]*) ;;
    *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
  esac
  version="${tag#v}"
  [ "${version%%[!0-9.]*}" = "$tree_version" ] || fail "the tag is $tag and Version.h says $tree_version"
else
  version="$tree_version"
fi
printf '%s\n' "$version" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z][0-9A-Za-z.-]*)?' \
  || fail "$version is not a SemVer 2 version, which Helm requires of a chart: tag a pre-release v<version>-<suffix>"
case "$version" in
  *-*) prerelease=true ;;
  *) prerelease=false ;;
esac
name="hmailserver-chart-$version.tgz"
echo "chart version and appVersion: $version (prerelease: $prerelease)"

rm -rf "$out"
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp -R "$chart" "$work/hmailserver"
stamped="$work/hmailserver/Chart.yaml"
# Each stamp replaces exactly one line, or the tree's Chart.yaml is not the one this
# was written for.
[ "$(grep -c "^      image: $image:latest\$" "$stamped")" = 1 ] || fail "Chart.yaml names the image $image:latest in artifacthub.io/images other than once"
[ "$(grep -c '^  artifacthub.io/prerelease: "false"$' "$stamped")" = 1 ] || fail "Chart.yaml has no artifacthub.io/prerelease: \"false\" line"
sed -i "s|^      image: $image:latest\$|      image: $image:$version|" "$stamped"
sed -i "s|^  artifacthub.io/prerelease: \"false\"\$|  artifacthub.io/prerelease: \"$prerelease\"|" "$stamped"

helm lint "$work/hmailserver" --set database.host=db --set database.password=x --set administrator.password=y
helm package "$work/hmailserver" --version "$version" --app-version "$version" --destination "$work/package"
[ -f "$work/package/hmailserver-$version.tgz" ] || fail "helm package wrote no hmailserver-$version.tgz"
mv "$work/package/hmailserver-$version.tgz" "$out/$name"

# Read back: the chart's own metadata, and what it renders.
helm show chart "$out/$name" > "$work/chart.txt"
cat "$work/chart.txt"
grep -Eqx "version: \"?$version\"?" "$work/chart.txt" || fail "the packaged chart's version is not $version"
grep -Eqx "appVersion: \"?$version\"?" "$work/chart.txt" || fail "the packaged chart's appVersion is not $version"
grep -Eq "image: $image:$version\$" "$work/chart.txt" || fail "the packaged chart's artifacthub.io/images does not name $image:$version"
grep -Eq "artifacthub.io/prerelease: \"?$prerelease\"?\$" "$work/chart.txt" || fail "the packaged chart's artifacthub.io/prerelease is not $prerelease"
helm template mail "$out/$name" --set database.host=db --set database.password=x --set administrator.password=y \
  --set rest.tlsSecretName=mail-tls > "$work/rendered.yaml"
grep -q "image: \"$image:$version\"" "$work/rendered.yaml" || fail "the packaged chart does not install $image:$version by default"
for kind in StatefulSet Service Secret; do
  grep -q "^kind: $kind\$" "$work/rendered.yaml" || fail "the packaged chart renders no $kind"
done
tar -tzf "$out/$name" | grep -qx 'hmailserver/README.md' || fail "the packaged chart carries no README.md, which Artifact Hub shows"

( cd "$out" && sha256sum "$name" > SHA256SUMS.txt && cat SHA256SUMS.txt )
ls -l "$out"
echo "PACKAGED: $out/$name, the chart $version, installing $image:$version."
