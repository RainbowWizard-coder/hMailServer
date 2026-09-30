#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The appimage-x86_64 and appimage-aarch64 jobs of .gitlab/ci/packages-appimage.yml:
# the server built on Ubuntu 24.04 (x86_64), or cross-compiled there by
# appimage-cross.sh and handed over (aarch64, HM_PREBUILT below), packed as an
# AppImage with the runtime the appimage-runtime-* job made, then run.
#
# Why Ubuntu 24.04: an AppImage runs where the glibc it was linked against, or a
# later one, is installed, so it is built on the oldest release that can build the
# server at all - Boost 1.83 or later (the CMakeLists asks for it), OpenSSL 3, C++20,
# CMake 3.22. Ubuntu 24.04 has Boost 1.83 and glibc 2.39. Debian 13 has the same
# Boost and glibc 2.41; Debian 12, Ubuntu 22.04, RHEL 9 and openSUSE Leap 15 have
# a Boost older than 1.83.
#
# Run in ubuntu:24.04 (the job's image, amd64 or arm64), from the root of a checkout:
#
#   bash build/ci/packages/appimage-build.sh
#
# It reads appimage/runtime-<arch> and runtime-<arch>-linked.txt, what the runtime
# links (the runtime job's artifacts), and appimage-runtime-sources/licenses-<arch>/,
# the licence files of the libraries linked into that runtime (the
# appimage-runtime-sources job's), which the image carries under
# usr/share/hmailserver/licenses/appimage-runtime/. build-appimage.sh puts in the
# image the copyright files of the Ubuntu packages whose libraries it copies into
# usr/lib, and bundled.txt, the list of them. It leaves:
#   appimage/hMailServer-<version>-<arch>.AppImage   the name UpdateChecker.cpp and
#                                                   generate-update-feed.py expect
#   appimage/SHA256SUMS-<arch>.txt
#   appimage/build-<arch>.log                        the whole ninja output
#   appimage/appimage-<arch>.txt                     what was built on what, and the
#                                                   libraries bundled and left to the host
#   appimage/built-against-<arch>/                   what the image's software bill of
#                                                   materials is made from, since this
#                                                   container has neither PowerShell nor
#                                                   Python: dpkg.txt and boost-version.hpp,
#                                                   what the server was compiled against
#                                                   (this container's for x86_64;
#                                                   appimage-cross.sh's for aarch64), and
#                                                   bundled.txt, each library the image
#                                                   carries in usr/lib with the package,
#                                                   version and SHA-256 of the file it came
#                                                   from and the source package and version
#                                                   it was built from (the image's own list
#                                                   with the SHA-256s), and runtime-linked.txt,
#                                                   what the runtime in front of it links
#                                                   statically. The release-packages job
#                                                   makes <image>.cdx.json from them
#                                                   (build/asset-sbom.py --kind appimage).
#                                                   And archive.txt, when this container's
#                                                   apt read Ubuntu's archive, the date of
#                                                   each index it read and when its dpkg
#                                                   installed each of those packages, for
#                                                   the appimage-bundled-sources job to find
#                                                   those source packages again
#                                                   (build/ci/packages/appimage-bundled-sources.sh).
#
# The environment it reads:
#   HM_BUILD_DIR    where CMake configures and ninja builds (/tmp/hm-appimage)
#   HM_BUILD_JOBS   ninja's -j (6)
#   HM_OUT_DIR      where the artifacts go, and where the runtime is (appimage)
#   HM_RUNTIME_LICENSES
#                   the runtime's libraries' licence files
#                   (appimage-runtime-sources/licenses-<arch>)
#   HM_CC, HM_CXX   the compilers (gcc, g++: Ubuntu 24.04's GCC 13)
#   HM_PREBUILT     a server already compiled for this architecture, which is
#                   packaged instead of building one: the aarch64 job's, which
#                   appimage-cross.sh cross-compiles (appimage/hmailserver-aarch64)
#                   and this script packs in an arm64 container under qemu-user
set -euo pipefail

build_dir="${HM_BUILD_DIR:-/tmp/hm-appimage}"
jobs="${HM_BUILD_JOBS:-6}"
out="${HM_OUT_DIR:-appimage}"
arch=$(uname -m)
runtime="$out/runtime-$arch"
runtime_licenses="${HM_RUNTIME_LICENSES:-appimage-runtime-sources/licenses-$arch}"
prebuilt="${HM_PREBUILT:-}"

if [ ! -f hmailserver/source/Server/CMakeLists.txt ]; then
  echo "Run this from the root of a checkout (no hmailserver/source/Server/CMakeLists.txt here)."
  exit 2
fi
case "$arch" in
  x86_64|aarch64) ;;
  *) echo "The AppImages are x86_64 and aarch64 only; this is $arch."; exit 2 ;;
esac
if [ ! -f "$runtime" ]; then
  echo "No runtime at $runtime: the appimage-runtime-$arch job makes it (build/ci/packages/appimage-runtime.sh)."
  exit 2
fi
if [ ! -s "$runtime-linked.txt" ]; then
  echo "No record of what the runtime links at $runtime-linked.txt, which the image's SBOM lists: the appimage-runtime-$arch job writes it."
  exit 2
fi
if [ ! -d "$runtime_licenses" ]; then
  echo "No licence files of the runtime's libraries at $runtime_licenses: the appimage-runtime-sources job makes them (build/ci/packages/appimage-runtime-sources.sh)."
  exit 2
fi
# shellcheck source=/dev/null
. /etc/os-release
if [ "$ID" != "ubuntu" ] || [ "$VERSION_ID" != "24.04" ]; then
  echo "This is $PRETTY_NAME. The AppImage is built on Ubuntu 24.04, for the oldest glibc that can build the server; run it in the image the job names."
  exit 2
fi

if [ -n "$prebuilt" ] && [ ! -f "$prebuilt" ]; then
  echo "HM_PREBUILT names $prebuilt, which does not exist: the appimage-$arch-compile job makes it."
  exit 2
fi

export DEBIAN_FRONTEND=noninteractive
# With a prebuilt server the compiler is not needed, but the -dev packages are
# still installed: they bring in the very libraries (Ubuntu 24.04's) the server
# was linked against, which ldd resolves and build-appimage.sh copies.
tools="build-essential cmake ninja-build"
[ -z "$prebuilt" ] || tools=""
echo "== the packages, from Ubuntu's archive"
apt-get update -qq
# When the indexes the libraries below come from were read: the snapshot of the
# archive at this time holds them, whatever the archive holds later (archive.txt).
archive_updated=$(date -u +%Y%m%dT%H%M%SZ)
# shellcheck disable=SC2086
apt-get install -y -qq -o Acquire::Retries=3 --no-install-recommends \
  $tools \
  libssl-dev zlib1g-dev libpq-dev libldap-dev \
  libboost-thread-dev libboost-chrono-dev libboost-system-dev \
  libboost-filesystem-dev libboost-regex-dev libboost-program-options-dev \
  binutils file patchelf squashfs-tools desktop-file-utils ca-certificates > /dev/null

if [ -n "${CI_COMMIT_TIMESTAMP:-}" ]; then
  SOURCE_DATE_EPOCH=$(date -d "$CI_COMMIT_TIMESTAMP" +%s)
  export SOURCE_DATE_EPOCH
elif command -v git > /dev/null 2>&1 && git rev-parse --git-dir > /dev/null 2>&1; then
  SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
  export SOURCE_DATE_EPOCH
fi
echo "SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-unset}"

if [ -n "$prebuilt" ]; then
  echo ""
  echo "== the server $prebuilt, compiled elsewhere"
  mkdir -p "$build_dir"
  install -m 0755 "$prebuilt" "$build_dir/hmailserver"
  compiler="prebuilt: $(sed -n 's/^cross-compiled on //p' "$out/compile-$arch.txt" 2> /dev/null || echo unknown)"
else
export CC="${HM_CC:-gcc}" CXX="${HM_CXX:-g++}"
compiler=$("$CXX" --version | head -n 1)
echo "$compiler"
cmake --version | head -n 1

echo ""
echo "== configure"
# Not wiped first: in the job the tree is new, and by hand an HM_BUILD_DIR kept
# between runs makes a second run a packaging run.
cmake -S hmailserver/source/Server -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr > "$out/configure-$arch.log" 2>&1 \
  || { cat "$out/configure-$arch.log"; echo "CONFIGURE FAILED"; exit 3; }
tail -n 3 "$out/configure-$arch.log"

echo ""
echo "== build the server (ninja -k 0 -j $jobs hmailserver)"
# The server alone: the tester programs linux-build builds and runs on the CI
# image are not part of an AppImage, and under qemu every file costs minutes.
started=$(date +%s)
set +e
ninja -C "$build_dir" -k 0 -j "$jobs" hmailserver > "$out/build-$arch.log" 2>&1
code=$?
set -e
echo "ninja exit $code after $(( $(date +%s) - started )) s"
if [ "$code" -ne 0 ]; then
  # No `| head`: it stops reading, and grep or sort, still writing a long list, then
  # dies of SIGPIPE, which pipefail and set -e made the script's end before the
  # message below (58710c9d6). grep -m stops reading the log itself, and sed reads
  # its input to the end; a log without the lines is not a second failure.
  grep -m 40 -E '^FAILED:' "$out/build-$arch.log" || true
  grep -E 'error:' "$out/build-$arch.log" | sort -u | sed -n '1,80p' || true
  echo "BUILD FAILED - the whole log is the job's artifact $out/build-$arch.log"
  exit 3
fi
fi
"$build_dir/hmailserver" --version

echo ""
echo "== the AppImage"
bash hmailserver/source/Server/platform/packaging/build-appimage.sh \
  --build-dir "$build_dir" --output-dir "$out" --runtime "$runtime" --runtime-licenses "$runtime_licenses" \
  | tee "$out/package-$arch.log"

image=$(find "$out" -maxdepth 1 -name "hMailServer-*-$arch.AppImage" | head -n 1)
[ -n "$image" ] || { echo "build-appimage.sh made no AppImage."; exit 1; }
version=$("$build_dir/hmailserver" --version | awk '{ print $2 }')
[ "$(basename "$image")" = "hMailServer-$version-$arch.AppImage" ] \
  || { echo "$(basename "$image") is not the name the update checker looks for, hMailServer-$version-$arch.AppImage."; exit 1; }

echo ""
echo "== it runs"
bash build/ci/packages/appimage-test.sh "$image"

echo ""
echo "== what it was built against, and what it carries, for its SBOM"
# dpkg's record in the form build/ci/packages/deb-build.sh writes and
# build/asset-sbom.py --dpkg-list reads. A server compiled here was compiled
# against this container's packages; the aarch64 one was cross-compiled by
# appimage-cross.sh, which recorded its own container's (the arm64 -dev packages
# and the cross toolchain), and that record is kept, not replaced by this one.
facts="$out/built-against-$arch"
mkdir -p "$facts"
if [ -z "$prebuilt" ]; then
  # shellcheck disable=SC2016
  dpkg-query -W -f '${db:Status-Abbrev} ${Package} ${Architecture} ${Version}\n' > "$facts/dpkg.txt"
  cp /usr/include/boost/version.hpp "$facts/boost-version.hpp"
fi
[ -s "$facts/dpkg.txt" ] && [ -s "$facts/boost-version.hpp" ] \
  || { echo "No record of what the server was compiled against in $facts (appimage-cross.sh writes the aarch64 one)."; exit 1; }
# Each library build-appimage.sh copied into usr/lib, from the image's own list of
# them (usr/share/hmailserver/licenses/bundled.txt: "library usr/lib/<soname>
# <package>:<arch> <version> <source> <source version>", each package as dpkg named
# the owner of the file ldd resolved), with that file's SHA-256: "<soname>
# <package>:<arch> <version> <sha256> <source> <source version>". The copy in the
# image has a RUNPATH of its own, so the package file's SHA-256 is recorded as that,
# not as the copy's.
list="$build_dir/AppDir/usr/share/hmailserver/licenses/bundled.txt"
[ -s "$list" ] || { echo "The image carries no usr/share/hmailserver/licenses/bundled.txt: build-appimage.sh writes it where dpkg is."; exit 1; }
bundled_names=$(sed -n 's/^Bundled://p' "$out/package-$arch.log" | head -n 1)
[ -n "$bundled_names" ] || { echo "build-appimage.sh said nothing of what it bundled."; exit 1; }
resolved="$(ldd "$build_dir/hmailserver")"
: > "$facts/bundled.txt"
listed=0
for soname in $bundled_names; do
  [ "$soname" != none ] || continue
  line=$(awk -v f="usr/lib/$soname" '$1 == "library" && $2 == f' "$list")
  [ "$(printf '%s\n' "$line" | grep -c .)" -eq 1 ] || { echo "The image's bundled.txt does not list usr/lib/$soname once."; exit 1; }
  read -r _ _ owner package_version source_package source_version extra <<< "$line"
  [ -n "$source_version" ] && [ -z "$extra" ] || { echo "The image's bundled.txt gives usr/lib/$soname as \"$line\"."; exit 1; }
  # A here-string, not printf into awk: awk exits at the match, and the shell's
  # printf of the rest of ldd's lines would then die of SIGPIPE, which pipefail
  # and set -e make the script's end.
  path=$(awk -v s="$soname" '$1 == s && $2 == "=>" { print $3; exit }' <<< "$resolved")
  [ -n "$path" ] && [ -f "$path" ] || { echo "ldd no longer resolves $soname, which the image carries."; exit 1; }
  printf '%s %s %s %s %s %s\n' "$soname" "$owner" "$package_version" "$(sha256sum "$(readlink -f "$path")" | cut -d' ' -f1)" \
    "$source_package" "$source_version" >> "$facts/bundled.txt"
  listed=$((listed + 1))
done
[ "$(grep -c '^library ' "$list")" -eq "$listed" ] \
  || { echo "The image's bundled.txt lists $(grep -c '^library ' "$list") libraries, and build-appimage.sh bundled $listed."; exit 1; }
cat "$facts/bundled.txt"
# What the runtime in front of the image links statically, as its job recorded it
# from the linker's map, for the SBOM.
cp "$runtime-linked.txt" "$facts/runtime-linked.txt"
# When this container's apt read Ubuntu's archive, and the date of each index it
# read (the InRelease files apt keeps): the appimage-bundled-sources job takes each
# source package from the archive, or, for a version the archive has dropped since,
# from its snapshot service at one of these times
# (build/ci/packages/appimage-bundled-sources.sh).
{
  echo "distribution $ID $VERSION_ID $VERSION_CODENAME"
  echo "arch $arch"
  echo "updated $archive_updated"
  for index in /var/lib/apt/lists/*_InRelease; do
    [ -f "$index" ] || continue
    suite=$(sed -n 's/^Suite: //p' "$index" | head -n 1)
    stamp=$(sed -n 's/^Date: //p' "$index" | head -n 1)
    [ -n "$suite" ] && [ -n "$stamp" ] || continue
    echo "index $suite $(date -u -d "$stamp" +%Y%m%dT%H%M%SZ)"
  done | LC_ALL=C sort -u
  # And when dpkg installed each package a library came from (its file list's time):
  # a package the image's base already had stays at the version it had when the base
  # was made, which the archive may have superseded before this apt read it, and only
  # the snapshot of that time still indexes its source.
  for owner in $(awk '{ print $2 }' "$facts/bundled.txt" | LC_ALL=C sort -u); do
    files="/var/lib/dpkg/info/$owner.list"
    [ -f "$files" ] || files="/var/lib/dpkg/info/${owner%%:*}.list"
    [ -f "$files" ] || { echo "dpkg keeps no file list of $owner, to say when it was installed." >&2; exit 1; }
    echo "installed $owner $(date -u -d "@$(stat -c %Y "$files")" +%Y%m%dT%H%M%SZ)"
  done
} > "$facts/archive.txt"
cat "$facts/archive.txt"
# Every source package among them is one the sources job knows the licence of: a
# library the image starts to carry is looked at before a release carries it.
bash build/ci/packages/appimage-bundled-sources.sh check "$facts"

( cd "$out" && sha256sum "$(basename "$image")" > "SHA256SUMS-$arch.txt" )
{
  basename "$image"
  echo "sha256 $(cut -d' ' -f1 "$out/SHA256SUMS-$arch.txt")"
  echo "size $(stat -c %s "$image")"
  echo "commit ${CI_COMMIT_SHA:-unknown}"
  echo "packed on $PRETTY_NAME ($arch), glibc $(ldd --version | head -n 1 | awk '{ print $NF }'); the server: $compiler"
  echo "boost $(dpkg-query -W -f '${Version}' libboost1.83-dev 2> /dev/null || echo unknown) (static)"
  grep -E '^(Bundled|Left to the host):' "$out/package-$arch.log"
  echo "their licences in usr/share/hmailserver/licenses/bundled/, their source $(sed -n 's/^sources //p' "$list")"
  echo "runtime $(sed -n 's/^sha256 //p' "$runtime.txt" 2> /dev/null || echo unknown)"
} | tee "$out/appimage-$arch.txt"
