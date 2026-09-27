#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The appimage-aarch64-compile job of .gitlab/ci/packages-appimage.yml: the server
# cross-compiled for aarch64 on Ubuntu 24.04, in an x86-64 container, against
# Ubuntu 24.04's own arm64 libraries (multiarch, from ports.ubuntu.com) - the same
# release, the same Boost 1.83 and the same glibc 2.39 as the x86_64 AppImage.
#
# Why cross and not native: linix, the project's runner, is x86-64. Compiled
# natively in an arm64 container under qemu-user, the server takes hours;
# cross-compiled it takes what the x86_64 build takes. What cannot be done
# cross is ldd, which runs the target's dynamic loader: the appimage-aarch64 job
# does the packaging in an arm64 container under qemu-user, where that takes
# minutes (build/ci/packages/appimage-build.sh with HM_PREBUILT).
#
# Run in ubuntu:24.04 for linux/amd64, from the root of a checkout:
#
#   bash build/ci/packages/appimage-cross.sh
#
# Leaves:
#   appimage/hmailserver-aarch64          the server, for appimage-build.sh
#   appimage/build-aarch64.log            the whole ninja output
#   appimage/compile-aarch64.txt          the compiler and the libraries built against
#
# The environment it reads: HM_BUILD_DIR (/tmp/hm-appimage-aarch64), HM_BUILD_JOBS (6),
# HM_OUT_DIR (appimage).
set -euo pipefail

build_dir="${HM_BUILD_DIR:-/tmp/hm-appimage-aarch64}"
jobs="${HM_BUILD_JOBS:-6}"
out="${HM_OUT_DIR:-appimage}"

if [ ! -f hmailserver/source/Server/CMakeLists.txt ]; then
  echo "Run this from the root of a checkout (no hmailserver/source/Server/CMakeLists.txt here)."
  exit 2
fi
. /etc/os-release
if [ "$ID" != "ubuntu" ] || [ "$VERSION_ID" != "24.04" ] || [ "$(uname -m)" != "x86_64" ]; then
  echo "This is $PRETTY_NAME on $(uname -m). It cross-compiles in Ubuntu 24.04 on x86-64; run it in the image the job names."
  exit 2
fi
mkdir -p "$out"

export DEBIAN_FRONTEND=noninteractive
echo "== arm64 as a foreign architecture, from ports.ubuntu.com"
# Ubuntu publishes amd64 on archive.ubuntu.com and security.ubuntu.com, and every
# other architecture on ports.ubuntu.com: the image's own sources are held to
# amd64, and a second file names the ports archive for arm64.
sed -i '/^Architectures:/d; /^Types: deb/a Architectures: amd64' /etc/apt/sources.list.d/ubuntu.sources
cat > /etc/apt/sources.list.d/ubuntu-ports.sources <<'SOURCES'
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports/
Suites: noble noble-updates noble-security
Components: main universe
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
SOURCES
dpkg --add-architecture arm64
apt-get update -qq
apt-get install -y -qq -o Acquire::Retries=3 --no-install-recommends \
  crossbuild-essential-arm64 cmake ninja-build file \
  libssl-dev:arm64 zlib1g-dev:arm64 libpq-dev:arm64 libldap-dev:arm64 \
  libboost-thread-dev:arm64 libboost-chrono-dev:arm64 libboost-system-dev:arm64 \
  libboost-filesystem-dev:arm64 libboost-regex-dev:arm64 > /dev/null

if [ -n "${CI_COMMIT_TIMESTAMP:-}" ]; then
  SOURCE_DATE_EPOCH=$(date -d "$CI_COMMIT_TIMESTAMP" +%s)
  export SOURCE_DATE_EPOCH
fi

aarch64-linux-gnu-g++ --version | head -n 1
cmake --version | head -n 1

echo ""
echo "== configure for aarch64"
# CMake takes the target's library directory (/usr/lib/aarch64-linux-gnu) from
# the compiler it is given (CMAKE_LIBRARY_ARCHITECTURE), and no x86-64 -dev
# package is installed, so there is nothing of the host's to find; the check
# after the configure step holds it to that.
cmake -S hmailserver/source/Server -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  > "$out/configure-aarch64.log" 2>&1 \
  || { cat "$out/configure-aarch64.log"; echo "CONFIGURE FAILED"; exit 3; }
grep -E 'Found (OpenSSL|Boost|PostgreSQL)' "$out/configure-aarch64.log" || true
if grep -E 'Found (OpenSSL|Boost|PostgreSQL)' "$out/configure-aarch64.log" | grep -q 'x86_64-linux-gnu'; then
  echo "CMake found an x86-64 library for an aarch64 build."
  exit 3
fi

echo ""
echo "== build the server (ninja -k 0 -j $jobs hmailserver)"
started=$(date +%s)
set +e
ninja -C "$build_dir" -k 0 -j "$jobs" hmailserver > "$out/build-aarch64.log" 2>&1
code=$?
set -e
echo "ninja exit $code after $(( $(date +%s) - started )) s"
if [ "$code" -ne 0 ]; then
  grep -E '^FAILED:' "$out/build-aarch64.log" | head -n 40
  grep -E 'error:' "$out/build-aarch64.log" | sort -u | head -n 80
  echo "BUILD FAILED - the whole log is the job's artifact $out/build-aarch64.log"
  exit 3
fi

file "$build_dir/hmailserver"
file "$build_dir/hmailserver" | grep -q 'ARM aarch64' || { echo "The binary is not an aarch64 ELF."; exit 3; }
cp "$build_dir/hmailserver" "$out/hmailserver-aarch64"
{
  echo "hmailserver-aarch64"
  echo "sha256 $(sha256sum "$out/hmailserver-aarch64" | cut -d' ' -f1)"
  echo "commit ${CI_COMMIT_SHA:-unknown}"
  echo "cross-compiled on $PRETTY_NAME (x86_64) with $(aarch64-linux-gnu-g++ --version | head -n 1)"
  for p in libboost1.83-dev libssl-dev libpq-dev libldap-dev zlib1g-dev libc6-dev; do
    echo "$p:arm64 $(dpkg-query -W -f '${Version}' "$p:arm64" 2> /dev/null || echo unknown)"
  done
} | tee "$out/compile-aarch64.txt"
