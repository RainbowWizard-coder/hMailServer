#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The deb-<distro>-<arch> jobs of .gitlab/ci/packages-deb.yml: the server built
# for one Debian or Ubuntu release and one architecture, in that release's own
# stock container, with that release's own compiler and -dev packages, through
# CMake and CPack, into a .deb whose dependencies are that release's.
#
# Why every release needs a build of its own: dpkg-shlibdeps writes the Depends
# line from the libraries the binary was linked against on the machine that
# built it, with that machine's package names and minimum versions - libc6 (>=
# the builder's glibc), libssl3 on one release and libssl3t64 on the next,
# libldap-2.5-0 or libldap2 - so a .deb made on Ubuntu 26.04 names packages, and
# versions of them, that Debian 12 does not have. (Boost is linked statically,
# so it is not in Depends at all: CMakeLists.txt, "Boost is linked STATICALLY".)
#
#   HM_DEB_DISTRO=debian13 HM_DEB_ARCH=arm64 bash build/ci/packages/deb-build.sh
#
# run from the root of a checkout, as root, in the release's container:
#   debian12      debian:12     (Boost 1.74 there: 1.83 is built from boost.org's
#                                release tarball, below)
#   debian13      debian:13
#   ubuntu24.04   ubuntu:24.04
#   ubuntu26.04   ubuntu:26.04
# The container is always the host's architecture (amd64 on linix). arm64 is
# CROSS-compiled in it: dpkg --add-architecture arm64, the release's own
# crossbuild-essential-arm64 and its :arm64 -dev packages, and the toolchain
# file beside this script. Nothing is run under emulation here; the package is
# run on arm64 by the install job (deb-install.sh), under qemu.
#
# The file name, by the project's naming rule (docs/Packages-deb.md):
#   ubuntu26.04 - the generic hmailserver_<v>_<arch>.deb, Version <v>, as the
#                 update checker (UpdateChecker.cpp) looks for it;
#   every other - hmailserver_<v>-1~<distro>_<arch>.deb, Version <v>-1~<distro>:
#                 CPack's own DEB-DEFAULT name for CPACK_DEBIAN_PACKAGE_RELEASE,
#                 so the control file's Version and the file's name are one
#                 value and neither is renamed after the fact.
#
# What it leaves (HM_OUT_DIR, packages-deb/<distro>-<arch> by default):
#   hmailserver_*.deb    the package
#   SHA256SUMS.txt       its hash
#   build-info.txt       the release, the architecture, the route, every tool's
#                        and library's version, the control fields and the times
#   configure.log, build.log, package.log, and testers.log on a native build
#
# The environment it reads:
#   HM_DEB_DISTRO, HM_DEB_ARCH  what to build (required)
#   HM_BUILD_DIR     where CMake builds (/tmp/hm-deb-build, in the container)
#   HM_BUILD_JOBS    ninja's -j (6)
#   HM_OUT_DIR       where the artifacts go
#   HM_BOOST_CACHE   debian12 only: where the Boost it builds is kept between
#                    runs (.cache/boost in the checkout, which the job caches)
set -uo pipefail

distro="${HM_DEB_DISTRO:-}"
arch="${HM_DEB_ARCH:-}"
out="${HM_OUT_DIR:-packages-deb/$distro-$arch}"
build_dir="${HM_BUILD_DIR:-/tmp/hm-deb-build}"
jobs="${HM_BUILD_JOBS:-6}"
boost_cache="${HM_BOOST_CACHE:-$PWD/.cache/boost}"

# Boost for Debian 12, whose own is 1.74 and the server asks for 1.83 or later
# (CMakeLists.txt). 1.83.0 is the oldest Boost the server is built against
# anywhere else (Ubuntu 24.04, Debian 13), so it is the one taken: the release
# tarball from boost.org's archive, held to the SHA-256 boost.org publishes
# beside it (boost_1_83_0.tar.bz2.json), and only the three compiled libraries
# the server links, statically, as it links every distribution's.
BOOST_VERSION=1.83.0
BOOST_URL=https://archives.boost.io/release/1.83.0/source/boost_1_83_0.tar.bz2
BOOST_SHA256=6478edfe2f3305127cffe8caf73ea0176c53769f4bf1585be237eb30798c3b8e

refuse() {
  echo "REFUSED: $*"
  exit 2
}
step() {
  echo ""
  echo "== $*"
}

case "$distro" in
  debian12 | debian13 | ubuntu24.04 | ubuntu26.04) ;;
  *) refuse "HM_DEB_DISTRO='$distro' is not one of debian12, debian13, ubuntu24.04, ubuntu26.04." ;;
esac
case "$arch" in
  amd64 | arm64) ;;
  *) refuse "HM_DEB_ARCH='$arch' is not amd64 or arm64." ;;
esac
[ -f hmailserver/source/Server/CMakeLists.txt ] \
  || refuse "run this from the root of a checkout (no hmailserver/source/Server/CMakeLists.txt here)."
[ "$(id -u)" = 0 ] || refuse "run this as root in the release's container: it installs the build's packages with apt."

# The name must not lie: the container is the release the file will be named after.
. /etc/os-release
[ "$ID$VERSION_ID" = "$distro" ] \
  || refuse "HM_DEB_DISTRO=$distro, but this container is $ID $VERSION_ID ($PRETTY_NAME)."
host=$(dpkg --print-architecture)
if [ "$arch" = "$host" ]; then
  route=native
elif [ "$host" = amd64 ] && [ "$arch" = arm64 ]; then
  route=cross
else
  refuse "a $arch package cannot be built in a $host container: arm64 is cross-compiled from amd64, and nothing else is."
fi

version=$(sed -n 's/^#define[[:space:]]*HMAILSERVER_VERSION[[:space:]]*"\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)".*/\1/p' \
  hmailserver/source/Server/Common/Application/Version.h)
[ -n "$version" ] || refuse "no HMAILSERVER_VERSION in Common/Application/Version.h."
if [ "$distro" = ubuntu26.04 ]; then
  deb_version="$version"
else
  deb_version="$version-1~$distro"
fi
expected="hmailserver_${deb_version}_${arch}.deb"

rm -rf "$out"
mkdir -p "$out"
started=$(date +%s)
echo "$PRETTY_NAME, $host container, $arch package ($route), $expected"

# The clock every stamping tool takes instead of now: the commit's, as linux-build.sh does.
if [ -n "${CI_COMMIT_TIMESTAMP:-}" ]; then
  SOURCE_DATE_EPOCH=$(date -d "$CI_COMMIT_TIMESTAMP" +%s)
  export SOURCE_DATE_EPOCH
elif command -v git > /dev/null 2>&1 && git rev-parse --git-dir > /dev/null 2>&1; then
  SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
  export SOURCE_DATE_EPOCH
fi

# ------------------------------------------------------------------ packages
step "the release's own toolchain and -dev packages"
export DEBIAN_FRONTEND=noninteractive
if [ "$route" = cross ]; then
  dpkg --add-architecture arm64
  if [ "$ID" = ubuntu ]; then
    # Ubuntu serves amd64 from archive.ubuntu.com and arm64 from ports.ubuntu.com
    # only, so the stock sources are held to amd64 and the ports added for arm64.
    # Deb822 since 24.04; both releases built here have it.
    sources=/etc/apt/sources.list.d/ubuntu.sources
    [ -f "$sources" ] || refuse "no $sources in this Ubuntu image."
    sed -i '/^Architectures:/d; s/^Types:.*/&\nArchitectures: amd64/' "$sources"
    cat > /etc/apt/sources.list.d/ubuntu-ports.sources << EOF
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports/
Suites: $VERSION_CODENAME $VERSION_CODENAME-updates $VERSION_CODENAME-security
Components: main universe
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
  fi
  # deb.debian.org serves every architecture from one mirror: nothing to add.
fi
suffix=""
[ "$route" = cross ] && suffix=":arm64"
dev="libssl-dev$suffix zlib1g-dev$suffix libpq-dev$suffix libldap-dev$suffix"
if [ "$distro" != debian12 ]; then
  dev="$dev libboost-thread-dev$suffix libboost-chrono-dev$suffix libboost-filesystem-dev$suffix"
fi
if [ "$route" = cross ]; then
  # The target's C++ runtime as the release's own arm64 packages, and not only
  # the cross toolchain's copy under /usr/aarch64-linux-gnu: dpkg-shlibdeps
  # writes libstdc++6 and libgcc-s1 into Depends from these packages' symbols
  # files, and without them fails "cannot find library libstdc++.so.6". Debian
  # 13 and Ubuntu pull them in with the Boost -dev packages; Debian 12, whose
  # Boost is built here, does not (found on the first Debian 12 arm64 trial).
  tools="crossbuild-essential-arm64 libstdc++6:arm64 libgcc-s1:arm64"
else
  tools="g++"
fi
# g++ is also wanted on a cross build of Debian 12: Boost's own build engine, b2,
# is compiled for the host before it compiles Boost for the target.
if [ "$distro" = debian12 ]; then
  tools="$tools bzip2 curl"
  [ "$route" = cross ] && tools="$tools g++"
fi
# apt-get update exits 0 when it could not fetch an index ("W: Failed to fetch",
# "W: Some index files failed to download"), and the install after it then fails
# on a package it cannot locate. Such a warning fails the update here, and the
# update is tried three times (deb-install.sh says where it was seen).
updated=""
for try in 1 2 3; do
  if apt-get update -qq > "$out/apt.log" 2>&1 \
      && ! grep -q -E '^(W: Failed to fetch|W: Some index files failed|E: )' "$out/apt.log"; then
    updated=yes
    break
  fi
  echo "apt-get update, try $try of 3: $(grep -E '^(W|E): ' "$out/apt.log" | head -n 1)"
  [ "$try" = 3 ] || sleep 15
done
[ -n "$updated" ] || { tail -n 20 "$out/apt.log"; echo "APT UPDATE FAILED"; exit 3; }
# An install that failed only to download is tried again, up to three times; one
# that failed otherwise (a package this release does not have) fails at once.
installed_ok=""
for try in 1 2 3; do
  # shellcheck disable=SC2086
  apt-get install -y -qq -o Acquire::Retries=3 --no-install-recommends \
    $tools cmake ninja-build file dpkg-dev ca-certificates $dev > /tmp/apt-install.log 2>&1
  code=$?
  cat /tmp/apt-install.log >> "$out/apt.log"
  if [ "$code" -eq 0 ]; then
    installed_ok=yes
    break
  fi
  grep -q -E 'Failed to fetch|Unable to connect|Temporary failure resolving|Could not resolve' /tmp/apt-install.log || break
  echo "apt-get install, try $try of 3: $(grep -E 'Failed to fetch|Unable to connect' /tmp/apt-install.log | head -n 1)"
  [ "$try" = 3 ] || sleep 15
done
if [ -z "$installed_ok" ]; then
  tail -n 40 "$out/apt.log"
  echo "APT INSTALL FAILED: the whole of it is $out/apt.log"
  exit 3
fi
echo "installed: $tools cmake ninja-build file dpkg-dev $dev"

if [ "$route" = cross ]; then
  export CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++
  # dpkg-shlibdeps, which CPack runs for the Depends line, reads the binary with
  # the target's objdump and looks up the arm64 libraries' shlibs only when told
  # the host architecture is arm64.
  export DEB_HOST_ARCH=arm64
  toolchain=(-DCMAKE_TOOLCHAIN_FILE="$PWD/build/ci/packages/aarch64-linux-gnu.cmake")
else
  export CC=gcc CXX=g++
  toolchain=()
fi

# ------------------------------------------------------------------ Boost for Debian 12
boost_args=()
boost_from="the release's own"
if [ "$distro" = debian12 ]; then
  prefix="$boost_cache/$BOOST_VERSION-$distro-$arch"
  step "Boost $BOOST_VERSION for Debian 12 $arch, from boost.org's release tarball"
  if [ -f "$prefix/.complete" ]; then
    echo "kept from an earlier run: $prefix"
  else
    boost_started=$(date +%s)
    rm -rf "$prefix" /tmp/boost-src
    mkdir -p /tmp/boost-src
    curl -fsSL --retry 3 -o /tmp/boost-src/boost.tar.bz2 "$BOOST_URL" || { echo "BOOST DOWNLOAD FAILED: $BOOST_URL"; exit 3; }
    echo "$BOOST_SHA256  /tmp/boost-src/boost.tar.bz2" | sha256sum -c - || { echo "BOOST TARBALL IS NOT THE ONE BOOST.ORG PUBLISHED"; exit 3; }
    tar -xjf /tmp/boost-src/boost.tar.bz2 -C /tmp/boost-src
    src=$(echo /tmp/boost-src/boost_*/)
    b2_toolset=()
    if [ "$route" = cross ]; then
      echo "using gcc : arm64 : aarch64-linux-gnu-g++ ;" > "$src/user-config.jam"
      b2_toolset=(toolset=gcc-arm64 architecture=arm address-model=64 --user-config="$src/user-config.jam")
    fi
    if ! ( cd "$src" && ./bootstrap.sh --with-toolset=gcc --with-libraries=thread,chrono,filesystem \
             && ./b2 -j"$jobs" -d0 "${b2_toolset[@]}" link=static threading=multi variant=release \
                  runtime-link=shared cxxflags=-fPIC cflags=-fPIC --prefix="$prefix" install ) > "$out/boost.log" 2>&1; then
      tail -n 40 "$out/boost.log"
      echo "BOOST BUILD FAILED: the whole of it is $out/boost.log"
      exit 3
    fi
    touch "$prefix/.complete"
    rm -rf /tmp/boost-src
    echo "built and installed in $prefix in $(( $(date +%s) - boost_started )) s"
  fi
  boost_args=(-DBoost_ROOT="$prefix" -DBoost_NO_SYSTEM_PATHS=ON)
  boost_from="boost.org $BOOST_VERSION, built here"
fi

# ------------------------------------------------------------------ what it is built with
libarch=$arch
boost_hpp=$(ls "${prefix:-/nonexistent}/include/boost/version.hpp" /usr/include/boost/version.hpp 2> /dev/null | head -n 1)
boost_version=$(sed -n 's/^#define BOOST_LIB_VERSION "\(.*\)"/\1/p' "$boost_hpp" | tr _ .)
{
  echo "distribution $PRETTY_NAME"
  echo "architecture $arch"
  echo "route $route ($host container)"
  echo "compiler $($CXX --version | head -n 1)"
  echo "cmake $(cmake --version | head -n 1 | sed 's/^cmake version //')"
  echo "boost $boost_version ($boost_from)"
  echo "openssl $(dpkg-query -W -f '${Version}' "libssl-dev:$libarch")"
  echo "libpq $(dpkg-query -W -f '${Version}' "libpq-dev:$libarch")"
  echo "openldap $(dpkg-query -W -f '${Version}' "libldap-dev:$libarch")"
  echo "zlib $(dpkg-query -W -f '${Version}' "zlib1g-dev:$libarch")"
} | tee "$out/build-info.txt"

# ------------------------------------------------------------------ build
step "configure"
rm -rf "$build_dir"
if ! cmake -S hmailserver/source/Server -B "$build_dir" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
      "${toolchain[@]}" "${boost_args[@]}" > "$out/configure.log" 2>&1; then
  cat "$out/configure.log"
  echo "CONFIGURE FAILED"
  exit 3
fi
grep -E 'Boost|OpenSSL|PostgreSQL|hMailServer version' "$out/configure.log" | head -n 10

step "build (ninja -k 0 -j $jobs)"
build_started=$(date +%s)
ninja -C "$build_dir" -k 0 -j "$jobs" > "$out/build.log" 2>&1
code=$?
build_seconds=$(( $(date +%s) - build_started ))
echo "ninja exit $code after $build_seconds s; $(grep -c 'warning:' "$out/build.log" || true) warning line(s)"
if [ "$code" -ne 0 ]; then
  grep -E '^FAILED:' "$out/build.log" | head -n 40
  grep -E 'error:' "$out/build.log" | sort -u | head -n 80
  echo "BUILD FAILED - the whole log is $out/build.log"
  exit 3
fi
server="$build_dir/hmailserver"
file "$server" | sed "s|$build_dir/||"
case "$arch:$(file -b "$server")" in
  amd64:*x86-64* | arm64:*aarch64*) ;;
  *) echo "FAILED: the binary is not an $arch executable: $(file -b "$server")"; exit 3 ;;
esac

failed=""
if [ "$route" = native ]; then
  # The tester programs the build makes (linux-build.sh runs the same), with this
  # release's compiler and libraries rather than the CI image's clang. A cross
  # build's are arm64 programs, and are not run here.
  step "the tester programs, with $($CXX --version | head -n 1)"
  "$server" --version || failed="$failed --version"
  : > "$out/testers.log"
  for program in $(find "$build_dir" -maxdepth 1 -type f -name '*-tests' -perm -u+x | sort); do
    name=$(basename "$program")
    ( cd "$build_dir" && timeout 600 "$program" ) >> "$out/testers.log" 2>&1
    rc=$?
    echo "$name exit $rc"
    [ "$rc" -eq 0 ] || failed="$failed $name"
  done
fi

# ------------------------------------------------------------------ package
step "the package: $expected"
release_args=()
if [ "$distro" != ubuntu26.04 ]; then
  release_args=(-D "CPACK_DEBIAN_PACKAGE_RELEASE=1~$distro")
fi
package_started=$(date +%s)
if ! ( cd "$build_dir" && cpack -G DEB "${release_args[@]}" ) > "$out/package.log" 2>&1; then
  tail -n 40 "$out/package.log"
  echo "CPACK FAILED: the whole of it is $out/package.log"
  exit 4
fi
made=$(cd "$build_dir" && ls -- *.deb 2> /dev/null)
if [ "$made" != "$expected" ]; then
  echo "FAILED: CPack made '$made', not $expected."
  exit 4
fi
cp "$build_dir/$made" "$out/"
deb="$out/$made"

# The control file agrees with the name, and names this release's libraries.
field() { dpkg-deb -f "$deb" "$1"; }
[ "$(field Package)" = hmailserver ] || { echo "FAILED: Package is '$(field Package)'"; exit 4; }
[ "$(field Version)" = "$deb_version" ] || { echo "FAILED: Version is '$(field Version)', not $deb_version"; exit 4; }
[ "$(field Architecture)" = "$arch" ] || { echo "FAILED: Architecture is '$(field Architecture)', not $arch"; exit 4; }
depends=$(field Depends)
[ -n "$depends" ] || { echo "FAILED: no Depends: dpkg-shlibdeps found none of the binary's libraries"; exit 4; }
case "$depends" in
  *libboost*) echo "FAILED: Depends names Boost, which is linked statically: $depends"; exit 4 ;;
esac
# Listed first and searched after: grep -q stops reading at its first match, and
# under pipefail the tar that dpkg-deb runs then dies of a broken pipe and fails
# the test it was passing (found on the first trial run on linix).
contents=$(dpkg-deb -c "$deb")
grep -q '\./usr/bin/hmailserver$' <<< "$contents" || { echo "FAILED: no /usr/bin/hmailserver in the package"; exit 4; }
( cd "$out" && sha256sum -- "$made" > SHA256SUMS.txt )
{
  echo "package $made"
  echo "version $(field Version)"
  echo "depends $depends"
  echo "installed-size $(field Installed-Size) KiB"
  echo "sha256 $(cut -d' ' -f1 "$out/SHA256SUMS.txt")"
  echo "commit ${CI_COMMIT_SHA:-unknown}"
  echo "seconds build $build_seconds, package $(( $(date +%s) - package_started )), job $(( $(date +%s) - started ))"
} | tee -a "$out/build-info.txt"

rm -rf "$build_dir"
echo ""
if [ -n "$failed" ]; then
  echo "FAILED:$failed (the package was made and is in $out; testers.log has the output)"
  exit 1
fi
echo "BUILT: $made for $PRETTY_NAME $arch ($route), in $(( $(date +%s) - started )) s."
