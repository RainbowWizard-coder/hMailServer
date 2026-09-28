#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The <build>-sbom jobs of .gitlab/ci/packages-deb.yml and packages-rpm.yml: the
# software bill of materials of a package a release carries, <package>.cdx.json
# beside it, made the way linux-build makes the x86-64 packages' own
# (build/ci/linux-build.sh): build/native-dependencies.ps1's component list, then
# build/asset-sbom.py make, which reads the package for what it declares it
# depends on (--read-package).
#
# What each library was built against is the one thing that must come from where
# the package was built, and linux-build asks its own dpkg (--query-dpkg). A
# package job builds in the distribution's own stock container, which has neither
# PowerShell nor Python, so the job records what that container had installed
# and the Boost header the build compiled against (deb-build.sh and rpm-build.sh,
# built-against/), and this reads the record (--dpkg-list, --rpm-list). It runs
# in the CI image, which has pwsh, python3, dpkg-deb and rpm, from the root of a
# checkout holding the build job's artifacts:
#
#   bash build/ci/packages/package-sbom.sh <package> <built-against directory>
#
#   bash build/ci/packages/package-sbom.sh packages-deb/ubuntu26.04-arm64/hmailserver_6.3.6_arm64.deb \
#        packages-deb/ubuntu26.04-arm64/built-against
#
# It writes <package>.cdx.json and holds it to the package (asset-sbom.py verify).
set -euo pipefail

fail() {
  echo "PACKAGE-SBOM FAILED: $*"
  exit 1
}

[ "$#" -eq 2 ] || fail "usage: $0 <package> <built-against directory> (given $# argument(s): a pattern that matched more than one package?)"
package=$1
facts=$2
[ -f hmailserver/source/Server/CMakeLists.txt ] || fail "run this from the root of a checkout"
[ -f "$package" ] || fail "$package is not here: the build job's artifact did not arrive, or it made no such package"
[ -d "$facts" ] || fail "$facts is not here: the build job recorded nothing the package was built against"

# The clock every stamping tool takes instead of now, as linux-build.sh sets it:
# the SBOM's timestamp is the commit's.
if [ -n "${CI_COMMIT_TIMESTAMP:-}" ]; then
  SOURCE_DATE_EPOCH=$(date -d "$CI_COMMIT_TIMESTAMP" +%s)
  export SOURCE_DATE_EPOCH
fi

case "$package" in
  *.deb)
    arch=$(dpkg-deb -f "$package" Architecture) || fail "dpkg-deb cannot read $package"
    [ -f "$facts/dpkg.txt" ] || fail "no $facts/dpkg.txt: the build job did not record its container's packages"
    record=(--kind deb --dpkg-list "$facts/dpkg.txt" --target-arch "$arch")
    ;;
  *.rpm)
    [ -f "$facts/rpm.txt" ] || fail "no $facts/rpm.txt: the build job did not record its rpm database"
    record=(--kind rpm --rpm-list "$facts/rpm.txt")
    ;;
  *) fail "$package is neither a .deb nor an .rpm" ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pwsh -NoProfile -File build/native-dependencies.ps1 -OutputFile "$work/native-dependencies.json" > /dev/null \
  || fail "build/native-dependencies.ps1 could not make the component list"
python3 build/asset-sbom.py make "${record[@]}" --asset "$package" --dependencies "$work/native-dependencies.json" \
  --boost-header "$facts/boost-version.hpp" --read-package \
  || fail "the SBOM of $package could not be made"
python3 build/asset-sbom.py verify "$package.cdx.json" "$package" || fail "$package.cdx.json does not describe $package"
echo "MADE: $package.cdx.json"
