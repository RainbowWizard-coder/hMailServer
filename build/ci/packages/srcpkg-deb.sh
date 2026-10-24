#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The srcpkg-<release> jobs of .gitlab/ci/source-packages.yml for Debian and
# Ubuntu: the Debian source package, built from the source tarball as an archive
# builds it (hmailserver/docs/SourcePackages.md).
#
#   HM_SRCPKG_TARGET=debian13 bash build/ci/packages/srcpkg-deb.sh
#
# run from the root of a checkout, as root, in that release's own container, with
# the source-tarball job's artifact in HM_SOURCE_TARBALL. The steps:
#
#   1. with the network: the packaging tools, and later the build's dependencies
#      as debian/control names them (apt-get build-dep);
#   2. the tarball repacked by mk-origtargz with debian/copyright's
#      Files-Excluded, as uscan repacks it: hmailserver_<v>+ds.orig.tar.gz, held
#      to lose exactly the files the Debian package leaves out;
#   3. debian/ taken from the tarball itself - hmailserver/source/Server/platform/
#      packaging/debian-src/debian - never from the checkout, so what is built is
#      what the tarball carries;
#   4. dpkg-buildpackage -us -uc under build/ci/packages/offline.c, which refuses
#      every socket but a local one, so a build that reaches for the network fails
#      (the job cannot use docker run --network none or unshare -n: offline.c says
#      why), and lintian over the result: an error fails the job, and the warnings
#      are listed in lintian.txt and in the log;
#   5. the binary package held to what the build was asked for: Boost in its
#      Depends (linked dynamically), and no live-update helper in it.
#
# What it leaves in HM_OUT_DIR (source-packages/<release>): the .dsc, the repacked
# .orig.tar.gz, the .debian.tar.xz, the two .deb files (not the debug symbols'),
# the .buildinfo and .changes, build.log, lintian.txt and build-info.txt. The
# install job (srcpkg-install.sh) takes them into a clean container.
#
# The environment it reads:
#   HM_SRCPKG_TARGET    debian13, ubuntu24.04 or ubuntu26.04 (required)
#   HM_SOURCE_TARBALL   the tarball, or a directory holding exactly one
#                       hmailserver-*.tar.gz (source-tarball)
#   HM_OUT_DIR          where the results go (source-packages/<release>)
#   HM_WORK_DIR         where it builds (/tmp/hm-srcpkg), emptied first
#   HM_BUILD_JOBS       dpkg-buildpackage -j (4)
set -uo pipefail

target="${HM_SRCPKG_TARGET:-}"
source_from="${HM_SOURCE_TARBALL:-source-tarball}"
out="${HM_OUT_DIR:-source-packages/$target}"
work="${HM_WORK_DIR:-/tmp/hm-srcpkg}"
jobs="${HM_BUILD_JOBS:-4}"
DEBIAN_DIR=hmailserver/source/Server/platform/packaging/debian-src/debian
# The files debian/copyright's Files-Excluded removes that the project's tarball
# carries: the test data that states no licence.
DEBIAN_ONLY="fuzz/regression/mime_decode_fuzzer/out-of-memory-mimeunicodeencoder-encodevalue
fuzz/regression/mime_message_fuzzer/heap-buffer-overflow-findstring-boundary-scan
hmailserver/test/RegressionTests/Resources/SecuniaBody1.txt
hmailserver/test/SSL examples/example+google.txt
hmailserver/test/SSL examples/google.cer
hmailserver/test/TestData/DKIM/Good/altn.response.txt
hmailserver/test/TestData/DKIM/Good/elandit.response.txt
hmailserver/test/TestData/DKIM/PermFail/port25.response.txt"

refuse() {
  echo "REFUSED: $*"
  exit 2
}
fail() {
  echo "FAILED: $*"
  exit 1
}
step() {
  echo ""
  echo "== $*"
}

case "$target" in
  debian13 | ubuntu24.04 | ubuntu26.04) ;;
  *) refuse "HM_SRCPKG_TARGET='$target' is not debian13, ubuntu24.04 or ubuntu26.04." ;;
esac
[ -f build/ci/packages/offline.c ] || refuse "run this from the root of a checkout (no build/ci/packages/offline.c here)."
[ "$(id -u)" = 0 ] || refuse "run this as root in the release's container: it installs the build's dependencies with apt."
. /etc/os-release
[ "$ID$VERSION_ID" = "$target" ] || refuse "HM_SRCPKG_TARGET=$target, but this container is $PRETTY_NAME."
here=$(pwd)

if [ -d "$source_from" ]; then
  found=$(find "$source_from" -maxdepth 1 -name 'hmailserver-*.tar.gz' | sort)
  [ "$(printf '%s' "$found" | grep -c .)" = 1 ] \
    || refuse "not exactly one hmailserver-*.tar.gz in $source_from (found: ${found:-none}); the source-tarball job's artifact is that directory."
  tarball=$found
else
  tarball=$source_from
fi
[ -f "$tarball" ] || refuse "no source tarball at $tarball."
tarball=$(cd "$(dirname "$tarball")" && pwd)/$(basename "$tarball")
name=$(basename "$tarball")
version=${name#hmailserver-}
version=${version%.tar.gz}
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || refuse "$name is not hmailserver-<major>.<minor>.<patch>.tar.gz."

case "$out" in /*) ;; *) out="$here/$out" ;; esac
case "$work" in
  / | "$here" | "$here"/*) refuse "HM_WORK_DIR=$work would empty the checkout." ;;
esac
rm -rf "$out" "$work"
mkdir -p "$out" "$work/meta"
started=$EPOCHSECONDS
echo "$PRETTY_NAME, $name, $jobs jobs"

# ------------------------------------------------------------------ with the network
step "the packaging tools, with the network"
export DEBIAN_FRONTEND=noninteractive
apt_opts=(-o Acquire::Retries=3 -o Acquire::ForceIPv4=true)
# apt-get update exits 0 when it could not fetch an index; such a warning fails the
# update here, and it is tried three times (deb-build.sh has the same loop).
apt_update() {
  local try
  for try in 1 2 3; do
    if apt-get update -qq "${apt_opts[@]}" > "$out/apt.log" 2>&1 \
        && ! grep -q -E '^(W: Failed to fetch|W: Some index files failed|E: )' "$out/apt.log"; then
      return 0
    fi
    echo "apt-get update, try $try of 3: $(grep -E '^(W|E): ' "$out/apt.log" | head -n 1)"
    [ "$try" = 3 ] || sleep 15
  done
  tail -n 20 "$out/apt.log"
  return 1
}
# An install that failed only to download is tried again, up to three times.
apt_retry() {
  local try code
  for try in 1 2 3; do
    "$@" > /tmp/hm-srcpkg-apt.log 2>&1
    code=$?
    cat /tmp/hm-srcpkg-apt.log >> "$out/apt.log"
    [ "$code" -eq 0 ] && return 0
    grep -q -E 'Failed to fetch|Unable to connect|Temporary failure resolving|Could not resolve' /tmp/hm-srcpkg-apt.log || break
    echo "try $try of 3: $(grep -E 'Failed to fetch|Unable to connect' /tmp/hm-srcpkg-apt.log | head -n 1)"
    [ "$try" = 3 ] || sleep 15
  done
  tail -n 30 /tmp/hm-srcpkg-apt.log
  return 1
}
apt_update || fail "apt-get update: $PRETTY_NAME's archive could not be read"
apt_retry apt-get install -y -qq "${apt_opts[@]}" --no-install-recommends \
    build-essential dpkg-dev devscripts lintian libdpkg-perl ca-certificates \
  || fail "the packaging tools did not install"
version_of() { dpkg-query -W -f '${Version}\n' "$1" 2> /dev/null | head -n 1; }
echo "dpkg-dev $(version_of dpkg-dev), lintian $(version_of lintian), devscripts $(version_of devscripts)"

step "the Debian packaging, from the tarball"
tar -xzf "$tarball" -C "$work/meta" "hmailserver-$version/$DEBIAN_DIR" \
  || fail "$name carries no $DEBIAN_DIR"
meta="$work/meta/hmailserver-$version/$DEBIAN_DIR"
deb_version=$(dpkg-parsechangelog -l "$meta/changelog" -S Version) || fail "debian/changelog does not parse"
upstream=${deb_version%-*}
echo "debian/changelog: $deb_version"
[ "$upstream" = "$version+ds" ] || fail "debian/changelog says $deb_version, and the tarball is $version: the upstream version must be $version+ds (the repack)"

step "the tarball repacked by Files-Excluded, as uscan repacks it"
cp "$tarball" "$work/"
if ! mk-origtargz --package hmailserver --version "$version" --repack-suffix +ds --compression gzip \
      --copyright-file "$meta/copyright" --directory "$work" "$work/$name" > "$out/repack.log" 2>&1; then
  cat "$out/repack.log"
  fail "mk-origtargz"
fi
cat "$out/repack.log"
orig="$work/hmailserver_$upstream.orig.tar.gz"
[ -f "$orig" ] || fail "mk-origtargz made no $(basename "$orig")"
tar -tzf "$tarball" | sed 's|^[^/]*/||' | grep -v '^$' | sort > "$work/before.txt"
tar -tzf "$orig" | sed 's|^[^/]*/||' | grep -v '^$' | sort > "$work/after.txt"
gone=$(comm -23 "$work/before.txt" "$work/after.txt")
added=$(comm -13 "$work/before.txt" "$work/after.txt")
expected=$(printf '%s\n' "$DEBIAN_ONLY" | sort)
[ -z "$added" ] || fail "the repack added entries: $added"
[ "$gone" = "$expected" ] || fail "the repack removed another set of files than the Debian-only eight:
$gone"
echo "$(basename "$orig"): $(wc -l < "$work/after.txt") entries, the eight without a licence gone"

step "the source tree, with debian/ from the tarball's own packaging"
rm -rf "$work/unpack"
mkdir "$work/unpack"
tar -xzf "$orig" -C "$work/unpack"
top=$(find "$work/unpack" -mindepth 1 -maxdepth 1)
[ "$(printf '%s' "$top" | grep -c .)" = 1 ] || fail "the repacked tarball has not one top directory"
src="$work/hmailserver-$upstream"
mv "$top" "$src"
cp -a "$src/$DEBIAN_DIR" "$src/debian"

step "the build's dependencies, as debian/control names them, with the network"
apt_retry apt-get build-dep -y -qq "${apt_opts[@]}" --no-install-recommends "$src" \
  || fail "apt-get build-dep: a Build-Depends that $PRETTY_NAME cannot meet"
{
  echo "distribution $PRETTY_NAME"
  echo "tarball $name $(sha256sum "$tarball" | cut -d' ' -f1)"
  echo "orig $(basename "$orig") $(sha256sum "$orig" | cut -d' ' -f1)"
  echo "compiler $(g++ --version | head -n 1)"
  echo "cmake $(cmake --version | head -n 1 | sed 's/^cmake version //')"
  # One line each: a package installed for two architectures answers twice.
  echo "debhelper $(dpkg-query -W -f '${Version}\n' debhelper | head -n 1)"
  echo "boost $(dpkg-query -W -f '${Version}\n' libboost-dev | head -n 1)"
  echo "openssl $(dpkg-query -W -f '${Version}\n' libssl-dev | head -n 1)"
} | tee "$out/build-info.txt"

# ------------------------------------------------------------------ without it
step "the network refused from here on (build/ci/packages/offline.c)"
cc -O2 -Wall -Wextra -o "$work/offline" build/ci/packages/offline.c || fail "offline.c did not compile"
if "$work/offline" getent ahosts deb.debian.org > /dev/null 2>&1; then
  fail "a name was looked up under offline: the network is not refused"
fi
echo "under offline: getent ahosts deb.debian.org fails, as every socket but a local one does"

step "dpkg-buildpackage -us -uc -j$jobs"
build_started=$EPOCHSECONDS
( cd "$src" && "$work/offline" dpkg-buildpackage -us -uc -j"$jobs" ) > "$out/build.log" 2>&1
code=$?
echo "exit $code after $(( EPOCHSECONDS - build_started )) s"
if [ "$code" -ne 0 ]; then
  # grep -m, not `| head`, which left grep to die of SIGPIPE under pipefail.
  grep -m 40 -E 'error:|^FAILED|dpkg-buildpackage: error|dh_[a-z_]*: error' "$out/build.log"
  tail -n 40 "$out/build.log"
  fail "dpkg-buildpackage; the whole log is build.log"
fi
grep -E '^== |PASSED|passed|failed' "$out/build.log" | grep -v '^-- ' | tail -n 12

step "lintian"
# Debian's profile on every release: the package is a Debian source package whose
# changelog names unstable, which Ubuntu's profile refuses as a distribution
# (bad-distribution-in-changes-file). A PPA upload names an Ubuntu series instead.
changes=$(ls "$work"/hmailserver_*.changes)
lintian --profile debian --fail-on error --tag-display-limit 0 --display-info "$changes" > "$out/lintian.txt" 2>&1
lintian_code=$?
errors=$(grep -c '^E: ' "$out/lintian.txt")
warnings=$(grep -c '^W: ' "$out/lintian.txt")
infos=$(grep -c '^I: ' "$out/lintian.txt")
echo "lintian $(dpkg-query -W -f '${Version}' lintian): $errors error(s), $warnings warning(s), $infos info tag(s); exit $lintian_code"
grep -E '^(E|W): ' "$out/lintian.txt" || true
[ "$errors" -eq 0 ] && [ "$lintian_code" -eq 0 ] || fail "lintian reports errors (lintian.txt)"

step "the packages"
# The debug symbols package (hmailserver-dbgsym, a .deb on Debian, a .ddeb on Ubuntu)
# is built and linted above and not kept: the install job does not read it, and each
# pipeline would upload it from linix and keep it for two weeks. The .changes still
# names it.
for file in "$work"/*.dsc "$orig" "$work"/*.debian.tar.* "$work"/*.deb "$work"/*.buildinfo "$changes"; do
  case "$(basename "$file")" in
    *-dbgsym_*) echo "built, linted, not kept: $(basename "$file") ($(du -h "$file" | cut -f1))" ;;
    *) cp "$file" "$out/" ;;
  esac
done
server_deb=$(ls "$out"/hmailserver_*_*.deb)
cli_deb=$(ls "$out"/hmailserver-cli_*_all.deb)
depends=$(dpkg-deb -f "$server_deb" Depends)
echo "Depends: $depends"
case "$depends" in
  *libboost*) ;;
  *) fail "Depends names no Boost library: the server was not linked against the distribution's shared Boost" ;;
esac
contents=$(dpkg-deb -c "$server_deb")
if grep -q 'hmailserver-update' <<< "$contents"; then
  fail "the package carries the live update's helper, which a build with HM_LIVE_UPDATE OFF does not install"
fi
grep -q '\./usr/lib/sysusers.d/hmailserver.conf$' <<< "$contents" || fail "no sysusers.d file in the package"
grep -q '\./usr/lib/tmpfiles.d/hmailserver.conf$' <<< "$contents" || fail "no tmpfiles.d file in the package"
grep -q '\./usr/bin/hmctl$' <<< "$(dpkg-deb -c "$cli_deb")" || fail "hmailserver-cli carries no hmctl"
( cd "$out" && sha256sum -- *.dsc *.orig.tar.gz *.debian.tar.* *.deb > SHA256SUMS.txt )
{
  echo "version $deb_version"
  echo "depends $depends"
  echo "lintian errors $errors warnings $warnings info $infos"
  echo "commit ${CI_COMMIT_SHA:-unknown}"
  echo "seconds $(( EPOCHSECONDS - started ))"
} | tee -a "$out/build-info.txt"
ls -l "$out"

rm -rf "$work"
echo ""
echo "BUILT: hmailserver $deb_version for $PRETTY_NAME from $name, with the network refused, lintian clean of errors ($warnings warning(s)), in $(( EPOCHSECONDS - started )) s."
