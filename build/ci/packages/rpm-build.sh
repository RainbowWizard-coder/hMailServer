#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The rpm-<distribution>-<arch> jobs of .gitlab/ci/packages-rpm.yml: the server
# built on one RPM distribution, with that distribution's own compiler, headers
# and libraries, and packaged by CPack as an .rpm whose automatic Requires name
# that distribution's own sonames. Run as root in the distribution's stock
# x86_64 container image (the job's image), from the root of a checkout:
#
#   bash build/ci/packages/rpm-build.sh <target> <arch>
#
#   target   fedora44 fedora43 el10 el9 leap16 tumbleweed
#   arch     x86_64 or aarch64
#
# By hand on a machine with docker it is the same, e.g.
#
#   docker run --rm -v "$PWD":/src -w /src registry.fedoraproject.org/fedora:44 \
#          bash build/ci/packages/rpm-build.sh fedora44 aarch64
#
# Why one build per distribution: an .rpm's Requires are the sonames the binary
# was linked against (libssl.so.3, libpq.so.5, libldap.so.2 and their symbol
# versions), and they are only right for the distribution that built it. Boost
# is linked statically (CMakeLists.txt says why), so the Boost a distribution
# ships matters at build time only: where it is older than 1.83 or ships no
# static archives, Boost 1.92.0 is built here from boost.org's own release
# tarball, checked against the SHA-256 libraries/build-boost.ps1 pins.
#
# AArch64 is CROSS-compiled, at the host's speed: the distribution's own clang,
# lld and rpmbuild in the x86_64 container, against a sysroot made of that same
# distribution release's aarch64 packages (glibc, libstdc++, gcc's start files,
# the -devel packages), installed into a directory by the distribution's own
# package manager for the other architecture. Nothing in the result comes from
# the x86_64 side but the compiler's code generation. The alternative, the
# whole build inside an aarch64 container under qemu-user, runs the compiler
# emulated and is several times slower (docs/PackagesRpm.md has the numbers).
# The package is then installed and started in a real aarch64 container of the
# same distribution by rpm-install-test.sh.
#
# What it leaves, for the job's artifacts (and nothing else):
#   packages-rpm/<target>-<arch>/hmailserver-<v>-1.<disttag>.<arch>.rpm
#   packages-rpm/fedora44-aarch64/generic/hmailserver-<v>-1.aarch64.rpm
#                               the same build under the generic name the update
#                               checker looks for (see the doc)
#   packages-rpm/<target>-<arch>/SHA256SUMS.txt, build-info.txt, requires.txt
#   packages-rpm/<target>-<arch>/built-against/
#                               what the package was built against, for its SBOM:
#                               rpm.txt, what the rpm database of the build held
#                               (the aarch64 sysroot's in a cross build), and
#                               boost-version.hpp, the Boost header the build
#                               compiled against. This stock container has neither
#                               PowerShell nor Python to make the SBOM itself;
#                               package-sbom.sh makes it from these in the CI image,
#                               for the packages a release carries (packages-rpm.yml)
#   packages-rpm/<target>-<arch>/*.log
#
# The environment it reads:
#   HM_BUILD_DIR    where CMake configures and ninja builds (/tmp/hm-rpm-build)
#   HM_BUILD_JOBS   ninja's and b2's -j (6)
#   HM_OUT_ROOT     where the artifacts go (packages-rpm)
#   HM_BOOST_TARBALL_CACHE
#                   where the Boost tarball is kept between runs, for the targets
#                   that build Boost (.cache/boost-tarball in the checkout, which
#                   their jobs cache under the tarball's SHA-256): fetch-boost.sh
set -uo pipefail

target="${1:-}"
arch="${2:-}"
build_dir="${HM_BUILD_DIR:-/tmp/hm-rpm-build}"
jobs="${HM_BUILD_JOBS:-6}"
out_root="${HM_OUT_ROOT:-packages-rpm}"
boost_tarball_cache="${HM_BOOST_TARBALL_CACHE:-$PWD/.cache/boost-tarball}"
fetch_boost="$(cd "$(dirname "$0")" && pwd)/fetch-boost.sh"

# .gitlab/ci/packages-rpm.yml's tarball cache is keyed by this SHA-256.
BOOST_VERSION=1.92.0
BOOST_SHA256=c4a3b310ddd2472416e091067166b0713be97c63f38c212c484ada022fd296ce

usage() {
  echo "usage: $0 <fedora44|fedora43|el10|el9|leap16|tumbleweed> <x86_64|aarch64>"
  exit 64
}

if [ ! -f hmailserver/source/Server/CMakeLists.txt ]; then
  echo "Run this from the root of a checkout (no hmailserver/source/Server/CMakeLists.txt here)."
  exit 2
fi

case "$arch" in
  x86_64|aarch64) ;;
  *) usage ;;
esac
# An aarch64 build runs its testers under qemu-user, and the install job after it runs
# the package in an aarch64 container, whose shell, an aarch64 program, cannot ask
# whether this machine runs aarch64 programs. Asked here, before the build
# (build/ci/arm64-emulation.sh), so that a missing qemu-aarch64 handler fails this job
# by name rather than leaving the testers unrun and the install job to an "exec format
# error".
if [ "$arch" = aarch64 ]; then
  sh build/ci/arm64-emulation.sh "rpm-$target-aarch64-install, which installs this package in an aarch64 container," || exit 2
fi

# ---------------------------------------------------------------- the targets
#
# family      the package manager
# os_id       what /etc/os-release must say, so that a job given the wrong image
#             fails at once instead of making a package under a wrong name
# disttag     the part of the release after "1." (the NAMING RULE of the doc)
# boost       distro: the distribution's own static Boost; source: 1.92.0 built here
# host_pkgs   what the x86_64 build container needs
# sys_pkgs    what the aarch64 sysroot needs (and, for a native build, what the
#             host needs on top of host_pkgs)
# post_req    Requires(post): what the %post scriptlet runs that a minimal
#             installation may lack - useradd and groupadd
common_tools="clang lld llvm cmake rpm-build gcc-c++ make tar gzip file findutils diffutils which"
case "$target" in
  fedora44|fedora43)
    family=dnf
    os_id="fedora ${target#fedora}"
    disttag="fc${target#fedora}"
    boost=distro
    host_pkgs="$common_tools ninja-build"
    sys_pkgs="glibc-devel libstdc++-devel gcc openssl-devel zlib-devel libpq-devel openldap-devel boost-devel boost-static"
    post_req="shadow-utils"
    ;;
  el10|el9)
    # Red Hat's Universal Base Image: RHEL's own packages, freely redistributable,
    # and its repositories carry every -devel package this build needs (clang, lld,
    # ninja-build in CodeReady Builder, openssl-devel, libpq-devel, openldap-devel)
    # but Boost. Boost is linked statically, so it is built here: EL9's own is
    # 1.75, older than CMakeLists.txt's 1.83, and neither UBI carries boost-devel.
    family=dnf
    os_id="rhel ${target#el}"
    disttag="$target"
    boost=source
    host_pkgs="$common_tools ninja-build"
    sys_pkgs="glibc-devel libstdc++-devel gcc openssl-devel zlib-devel libpq-devel openldap-devel"
    post_req="shadow-utils"
    ;;
  leap16)
    family=zypper
    os_id="opensuse-leap 16.0"
    disttag="suse.lp160"
    boost=source
    host_pkgs="$common_tools ninja cpio"
    # Leap 16 names OpenLDAP's headers openldap2_6-devel; Tumbleweed openldap2-devel.
    sys_pkgs="glibc-devel libstdc++-devel gcc gcc-c++ libopenssl-devel zlib-devel postgresql-devel openldap2_6-devel"
    post_req="shadow"
    ;;
  tumbleweed)
    family=zypper
    os_id="opensuse-tumbleweed"
    disttag="suse.tw"
    boost=source
    host_pkgs="$common_tools ninja cpio"
    sys_pkgs="glibc-devel libstdc++-devel gcc gcc-c++ libopenssl-devel zlib-devel postgresql-devel openldap2-devel"
    post_req="shadow"
    ;;
  *)
    usage
    ;;
esac

out="$out_root/$target-$arch"
rm -rf "$out"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
started_all=$(date +%s)
# What the package is built against, recorded for its SBOM (build/asset-sbom.py
# --rpm-list): one line per thing each package provides, since Fedora's zlib-devel
# is another package's. Inside [...] a tag that is not an array is written %{=TAG},
# or rpm prints "array iterator used with different sized arrays", nothing else,
# and exits 0 (rpm 6.0.1, 28 September 2026).
facts="$out/built-against"
mkdir -p "$facts"
rpm_format='[%{=NAME} %{=ARCH} %{=VERSION}-%{=RELEASE} %{PROVIDENAME}\n]'

step() {
  echo ""
  echo "== $* ($(( $(date +%s) - started_all )) s)"
}

. /etc/os-release
if [ "$os_id" != "$ID ${VERSION_ID:-}" ] && [ "$os_id" != "$ID" ]; then
  # EL: VERSION_ID is 10.0 or 9.6; the major version is what is meant.
  if [ "$os_id" != "$ID ${VERSION_ID%%.*}" ]; then
    echo "REFUSED: $target is built in its own distribution's image, and this is $PRETTY_NAME ($ID ${VERSION_ID:-})."
    exit 2
  fi
fi
if [ "$(uname -m)" != x86_64 ]; then
  echo "REFUSED: the build container is x86_64 for both architectures (aarch64 is cross-compiled); this is $(uname -m)."
  exit 2
fi
echo "$PRETTY_NAME, building for $arch, release 1.$disttag"

# ---------------------------------------------------------------- the host's packages
# A package manager's step, tried up to three times when it failed only to reach
# the repository (a mirror that did not answer, a name that did not resolve), and
# failed at once otherwise: a package the release does not have is not going to
# appear. apt in the Debian jobs on linix met such failures, most often under
# qemu (docs/PackagesDeb.md); dnf and zypper fetch through libcurl, which falls
# back from an unreachable IPv6 address by itself, and no run here has met one,
# so this is only the retry. Its output goes to the log it is given.
network_failure='Curl error|Cannot download|Failed to download|Download \(curl\) error|Timeout exceeded|Problem retrieving|Could not resolve|Couldn.t resolve|Couldn.t connect|Connection (refused|timed out|reset)'
retry_fetch() {
  local log=$1 try
  shift
  for try in 1 2 3; do
    "$@" > "$log" 2>&1 && return 0
    grep -q -E "$network_failure" "$log" || return 1
    echo "the repository could not be reached, try $try of 3: $(grep -m 1 -E "$network_failure" "$log")"
    [ "$try" = 3 ] || sleep 15
  done
  return 1
}

step "the build container's packages"
# zypper refreshes a repository it installs from by itself. Nothing auto-imports
# a key: the image already trusts its distribution's keys, and a package built
# for a release must not come from a repository that presented a new one.
install_host() {
  case "$family" in
    dnf) retry_fetch "$out/host-packages.log" dnf -y -q --setopt=install_weak_deps=False install "$@" ;;
    zypper) retry_fetch "$out/host-packages.log" zypper -n -q install --no-recommends "$@" ;;
  esac
}
# A native build needs the target's -devel packages on the host itself; a cross
# build needs them only in the sysroot, and the host's C++ library to build b2.
if [ "$arch" = x86_64 ]; then
  wanted="$host_pkgs $sys_pkgs"
else
  wanted="$host_pkgs"
fi
# The two -devel packages a cross build's b2 needs natively come with gcc-c++.
# shellcheck disable=SC2086
install_host $wanted || { tail -n 40 "$out/host-packages.log"; echo "FAILED: the build container's packages did not install."; exit 3; }
clang --version | head -n 1
cmake --version | head -n 1
rpmbuild --version

# ---------------------------------------------------------------- the aarch64 sysroot
sysroot=""
triple=""
if [ "$arch" = aarch64 ]; then
  sysroot=/tmp/hm-sysroot-aarch64
  step "the aarch64 sysroot: $PRETTY_NAME's own aarch64 packages"
  rm -rf "$sysroot"
  mkdir -p "$sysroot"
  case "$family" in
    dnf)
      # --forcearch installs another architecture's packages; noscripts because
      # their scriptlets are aarch64 programs and a sysroot needs only the files.
      # The repositories are the container's own, with $basearch now aarch64.
      dnf_major=$(dnf --version 2> /dev/null | sed -n 's/^dnf5 version \([0-9]*\).*/5/p; s/^\([0-9]\)\..*/\1/p' | head -n 1)
      host_config=()
      [ "$dnf_major" = 5 ] && host_config=(--use-host-config)
      # The merged-/usr symlinks (/lib64 -> usr/lib64 and the rest) are made by
      # the filesystem package's %pretrans, which noscripts skips. Without them
      # a package's /lib64 path makes a real directory there first and the
      # filesystem package then fails to unpack; with the links but not their
      # targets, libgcc fails to unpack into /lib64 (both on Fedora 44, 26
      # September 2026). So the links are copied from this container, which is
      # the same release, and their targets made.
      for link in bin sbin lib lib64 usr/sbin; do
        if [ -L "/$link" ]; then
          mkdir -p "$sysroot/$(dirname "$link")/$(readlink "/$link")"
          ln -s "$(readlink "/$link")" "$sysroot/$link"
        fi
      done
      # shellcheck disable=SC2086
      retry_fetch "$out/sysroot.log" dnf -y -q "${host_config[@]}" --installroot="$sysroot" --forcearch=aarch64 \
          --releasever="${VERSION_ID%%.*}" --setopt=install_weak_deps=False \
          --setopt=tsflags=noscripts,nodocs install $sys_pkgs \
        || { tail -n 40 "$out/sysroot.log"; echo "FAILED: the aarch64 sysroot did not install."; exit 3; }
      ;;
    zypper)
      # zypper has no --forcearch, but libzypp takes its architecture from
      # zypp.conf, and ZYPP_CONF names another one. The packages are downloaded
      # (their dependency closure, into an empty root) and unpacked, not
      # installed: a sysroot needs the files, and their scriptlets are aarch64.
      # Tumbleweed's aarch64 packages are a separate "ports" tree; Leap 16's
      # repositories hold every architecture.
      zypp_conf=/tmp/zypp-aarch64.conf
      printf '[main]\narch = aarch64\n' > "$zypp_conf"
      zroot=/tmp/hm-zypper-root
      rm -rf "$zroot"
      mkdir -p "$zroot/etc/zypp/repos.d"
      if [ "$target" = tumbleweed ]; then
        oss=https://download.opensuse.org/ports/aarch64/tumbleweed/repo/oss/
      else
        oss=https://download.opensuse.org/distribution/leap/16.0/repo/oss/aarch64/
      fi
      {
        # The keys this container already trusts, and no other: nothing is
        # auto-imported from the repository it is meant to verify.
        rpm --root "$zroot" --import /usr/lib/rpm/gnupg/keys/*.asc
        ZYPP_CONF=$zypp_conf zypper -n --root "$zroot" addrepo "$oss" oss
      } > "$out/sysroot.log" 2>&1 || { tail -n 40 "$out/sysroot.log"; echo "FAILED: the aarch64 repository could not be added."; exit 3; }
      retry_fetch "$out/sysroot-refresh.log" env ZYPP_CONF="$zypp_conf" zypper -n --root "$zroot" refresh \
        || { tail -n 40 "$out/sysroot-refresh.log"; echo "FAILED: the aarch64 repository's metadata did not download."; exit 3; }
      # shellcheck disable=SC2086
      retry_fetch "$out/sysroot-download.log" env ZYPP_CONF="$zypp_conf" \
          zypper -n --root "$zroot" install --no-recommends --download-only $sys_pkgs \
        || { tail -n 40 "$out/sysroot-download.log"; echo "FAILED: the aarch64 packages did not download."; exit 3; }
      count=0
      while IFS= read -r -d '' rpm; do
        case "$rpm" in *.aarch64.rpm|*.noarch.rpm) ;; *) echo "FAILED: $rpm is not aarch64"; exit 3 ;; esac
        ( cd "$sysroot" && rpm2cpio "$rpm" | cpio -idm --quiet 2> /dev/null )
        # Unpacked, not installed, so no rpm database records it: the list is
        # taken from each package as it goes in. zypper verified it when it
        # downloaded it, against the repository's metadata signed by a key imported
        # above; this container's own rpm database need not hold that key, and an
        # rpm that enforces signatures on reading would otherwise list nothing.
        rpm -qp --nosignature --qf "$rpm_format" "$rpm" >> "$facts/rpm.txt" 2> /dev/null
        count=$((count + 1))
      done < <(find "$zroot/var/cache/zypp/packages" -name '*.rpm' -print0)
      echo "$count aarch64 packages unpacked" >> "$out/sysroot.log"
      rm -rf "$zroot"
      ;;
  esac
  # GCC's own directory: /usr/lib/gcc/aarch64-redhat-linux on Fedora and RHEL,
  # /usr/lib64/gcc/aarch64-suse-linux on openSUSE. Its name is the triple clang
  # targets, and clang finds the start files and libstdc++ under either.
  gcc_dir=""
  triple=""
  for dir in "$sysroot/usr/lib/gcc" "$sysroot/usr/lib64/gcc"; do
    for candidate in "$dir"/aarch64*; do
      [ -d "$candidate" ] || continue
      gcc_dir=$candidate
      triple=$(basename "$candidate")
      break 2
    done
  done
  if [ -z "$triple" ] || [ ! -f "$sysroot/usr/include/openssl/ssl.h" ] || [ ! -f "$sysroot/usr/include/ldap.h" ]; then
    tail -n 20 "$out/sysroot.log"
    echo "FAILED: the sysroot has no aarch64 gcc installation, OpenSSL or OpenLDAP headers."
    exit 3
  fi
  echo "sysroot $(du -sh "$sysroot" | cut -f1), GCC installation ${gcc_dir#"$sysroot"}/$(ls "$gcc_dir" | head -n 1)"
fi

# ---------------------------------------------------------------- the hardening
# The core of what Fedora's, RHEL's and openSUSE's own packages are built with,
# and of what rpmbuild's %build would have added had the server been built
# there: a position-independent executable, full RELRO, the stack protector,
# stack clash protection and _FORTIFY_SOURCE. At level 2: Fedora's own packages
# now take level 3 (redhat-rpm-config's %_fortify_level) and add more, such as
# _GLIBCXX_ASSERTIONS, and neither was tried here, where the packages that
# passed were built at 2. CMake is not run from a spec here, so nothing
# adds them unless this does, and Fedora's and RHEL's clang link a non-PIE
# executable by default: the first builds of 26 September 2026 were "LSB
# executable" on Fedora 44 and UBI 9, x86_64 and aarch64 alike, and "LSB pie
# executable" only on Leap 16. CMakeLists.txt compiles with -fPIE already
# (CMAKE_POSITION_INDEPENDENT_CODE); the link is what differed. Every target
# builds no shared library, so -pie is for the executables alone. -fPIE is
# given to every compile as well, as redhat-rpm-config's hardening does: CMake's
# own test programs (the compiler ABI check, find_package's probes) do not get
# CMAKE_POSITION_INDEPENDENT_CODE, and without it their link with -pie fails
# (Fedora 44, 26 September 2026), which leaves the ABI undetected and lib64
# unsearched ("Could NOT find ZLIB"). The libraries' own -fPIC comes after it on
# their command lines and wins.
harden_cflags="-fPIE -fstack-protector-strong -fstack-clash-protection -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2"
harden_exe_ldflags="-pie -Wl,-z,relro -Wl,-z,now"

# ---------------------------------------------------------------- the toolchain
# lld, unlike GNU ld, obeys the .deplibs section clang writes for a
# "#pragma comment(lib, ...)" on ELF, and Parsing/StringParser.cpp names
# Normaliz.lib that way without an _WIN32 guard: the link fails with "unable to
# find library from dependent library specifier: Normaliz.lib" (Fedora 44
# aarch64, 26 September 2026). --no-dependent-libraries makes lld ignore the
# section, which is what GNU ld does in the native builds.
toolchain=""
if [ "$arch" = aarch64 ]; then
  toolchain=/tmp/hm-aarch64.cmake
  cat > "$toolchain" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT $sysroot)
set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET $triple)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_COMPILER_TARGET $triple)
set(CMAKE_AR $(command -v llvm-ar))
set(CMAKE_RANLIB $(command -v llvm-ranlib))
set(CMAKE_STRIP $(command -v llvm-strip))
set(CMAKE_C_FLAGS_INIT "$harden_cflags")
set(CMAKE_CXX_FLAGS_INIT "$harden_cflags")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -Wl,--no-dependent-libraries $harden_exe_ldflags")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -Wl,--no-dependent-libraries")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld -Wl,--no-dependent-libraries")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
fi

# ---------------------------------------------------------------- Boost
boost_args=()
if [ "$boost" = source ]; then
  step "Boost $BOOST_VERSION from boost.org's release tarball (thread, chrono, filesystem; static)"
  underscored=${BOOST_VERSION//./_}
  tarball=/tmp/boost_$underscored.tar.gz
  # From the job's cache, boost.org's archive or SourceForge's copy of it, and
  # only with the SHA-256 libraries/build-boost.ps1 pins (fetch-boost.sh).
  if ! bash "$fetch_boost" "$BOOST_VERSION" tar.gz "$BOOST_SHA256" "$tarball" "$boost_tarball_cache"; then
    echo "FAILED: no source gave boost_$underscored.tar.gz with the SHA-256 libraries/build-boost.ps1 pins."
    exit 3
  fi
  rm -rf "/tmp/boost_$underscored"
  tar -xzf "$tarball" -C /tmp
  rm -f "$tarball"
  # Installed where the build will look: /opt/boost as the compiler sees it,
  # which for a cross build is inside the sysroot.
  boost_prefix="$sysroot/opt/boost"
  (
    set -e
    cd "/tmp/boost_$underscored"
    ./bootstrap.sh --with-toolset=clang > "$out/boost.log" 2>&1
    b2_target=(toolset=clang)
    if [ "$arch" = aarch64 ]; then
      echo "using clang : aarch64 : clang++ : <compileflags>\"--target=$triple --sysroot=$sysroot\" <linkflags>\"--target=$triple --sysroot=$sysroot -fuse-ld=lld\" <archiver>$(command -v llvm-ar) <ranlib>$(command -v llvm-ranlib) ;" > user-config.jam
      b2_target=(--user-config=user-config.jam toolset=clang-aarch64 target-os=linux architecture=arm address-model=64 binary-format=elf abi=aapcs)
    fi
    hardening=()
    for flag in $harden_cflags; do hardening+=("cxxflags=$flag" "cflags=$flag"); done
    ./b2 -j"$jobs" -d1 "${b2_target[@]}" link=static threading=multi variant=release \
         cxxflags=-fPIC cflags=-fPIC "${hardening[@]}" cxxstd=20 \
         --with-thread --with-chrono --with-filesystem \
         --prefix="$boost_prefix" --build-dir=/tmp/boost-build install >> "$out/boost.log" 2>&1
  ) || { tail -n 40 "$out/boost.log"; echo "FAILED: Boost $BOOST_VERSION did not build."; exit 3; }
  rm -rf "/tmp/boost_$underscored" /tmp/boost-build
  ls "$boost_prefix/lib"/libboost_*.a | sed 's|^|  |'
  boost_args=(-DBoost_ROOT=/opt/boost)
fi

# ---------------------------------------------------------------- configure and build
if [ -n "${CI_COMMIT_TIMESTAMP:-}" ]; then
  SOURCE_DATE_EPOCH=$(date -d "$CI_COMMIT_TIMESTAMP" +%s)
  export SOURCE_DATE_EPOCH
elif command -v git > /dev/null 2>&1 && git rev-parse --git-dir > /dev/null 2>&1; then
  SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
  export SOURCE_DATE_EPOCH
fi
echo "SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-unset}"

step "configure"
rm -rf "$build_dir"
# Fedora's and EL's redhat-rpm-config rewrite "#!/usr/bin/env python3" into
# "#!/usr/bin/python3", which makes python3 a hard Requires of the package;
# CMakeLists.txt makes it a suggestion on purpose (hmctl is optional), so the
# rewrite is switched off and the shebang kept. CPack writes these lines into
# the spec's preamble as they are (the package step adds one).
more_define="%undefine __brp_mangle_shebangs"
cmake_args=(-S hmailserver/source/Server -B "$build_dir" -G Ninja
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
            "-DCPACK_RPM_PACKAGE_RELEASE=1.$disttag"
            "-DCPACK_RPM_PACKAGE_REQUIRES_POST=$post_req"
            "-DCPACK_RPM_SPEC_MORE_DEFINE=$more_define"
            "${boost_args[@]}")
if [ "$arch" = x86_64 ]; then
  export CC=clang CXX=clang++
  cmake_args+=("-DCMAKE_C_FLAGS=$harden_cflags" "-DCMAKE_CXX_FLAGS=$harden_cflags"
               "-DCMAKE_EXE_LINKER_FLAGS=$harden_exe_ldflags")
else
  # The toolchain file carries the same flags, as its _INIT values beside lld's.
  cmake_args+=("-DCMAKE_TOOLCHAIN_FILE=$toolchain")
fi
if ! cmake "${cmake_args[@]}" > "$out/configure.log" 2>&1; then
  cat "$out/configure.log"
  echo "CONFIGURE FAILED"
  exit 3
fi
grep -E 'hMailServer version|Boost|OpenSSL|PostgreSQL' "$out/configure.log" | head -n 8
echo "hardening: $harden_cflags; the executables linked $harden_exe_ldflags"

step "build (ninja -k 0 -j $jobs)"
started=$(date +%s)
ninja -C "$build_dir" -k 0 -j "$jobs" > "$out/build.log" 2>&1
code=$?
echo "ninja exit $code after $(( $(date +%s) - started )) s; $(grep -c 'warning:' "$out/build.log" || true) warning line(s)"
if [ "$code" -ne 0 ]; then
  # No `| head`: it stops reading, and grep or sort, still writing a long list, then
  # dies of SIGPIPE, which pipefail makes the status (58710c9d6). grep -m stops
  # reading the log itself, and sed reads its input to the end.
  grep -m 40 -E '^FAILED:' "$out/build.log"
  grep -E 'error:' "$out/build.log" | sort -u | sed -n '1,80p'
  echo "BUILD FAILED - the whole log is the job's artifact $out_root/$target-$arch/build.log"
  exit 3
fi

server="$build_dir/hmailserver"
file "$server" | sed "s|$build_dir/||"

# ---------------------------------------------------------------- the programs themselves
failed=""
# An aarch64 program runs here only where the kernel hands aarch64 ELF files to
# qemu (binfmt_misc, registered with the F flag so that it works inside a
# container), with the sysroot as its library root. That is asked of the kernel
# by running the server, not of /proc/sys/fs/binfmt_misc, which a container does
# not mount: bash answers 126 when the kernel cannot execute it. Without
# emulation the testers are not run, and this says so; the install test runs
# the packaged server on real aarch64 userland in any case.
can_run=yes
if [ "$arch" = aarch64 ]; then
  export QEMU_LD_PREFIX="$sysroot"
  "$server" --help > /dev/null 2>&1
  [ $? -ne 126 ] || can_run=""
fi
if [ -n "$can_run" ]; then
  step "the service answers for itself"
  if "$server" --version > "$out/version.txt" 2>&1 && "$server" --help > /dev/null 2>&1; then
    cat "$out/version.txt"
  else
    cat "$out/version.txt"
    failed="$failed hmailserver--version/--help"
  fi
  testers=$(find "$build_dir" -maxdepth 1 -type f -name '*-tests' -perm -u+x | sort)
  [ -n "$testers" ] || failed="$failed no-testers"
  for program in $testers; do
    name=$(basename "$program")
    ( cd "$build_dir" && timeout 900 "$program" ) > "$out/$name.log" 2>&1
    code=$?
    echo "$name exit $code ($(tail -n 1 "$out/$name.log"))"
    [ "$code" -eq 0 ] || { tail -n 30 "$out/$name.log"; failed="$failed $name"; }
  done
else
  step "the testers are NOT run: this kernel cannot execute an aarch64 program (no qemu-aarch64 in binfmt_misc)"
  echo "(rpm-install-test.sh runs the packaged server in an aarch64 container)"
fi

# ---------------------------------------------------------------- the package
step "the package"
make_package() {
  ( cd "$build_dir" && cpack -G RPM "$@" ) >> "$out/package.log" 2>&1
}
bash build/check-linux-version-stamp.sh > "$out/package.log" 2>&1 \
  || { tail -n 40 "$out/package.log"; echo "PACKAGING FAILED: the version stamps disagree"; exit 4; }
make_package || { tail -n 40 "$out/package.log"; echo "PACKAGING FAILED"; exit 4; }
version=$(sed -n 's/^#define[[:space:]]*HMAILSERVER_VERSION[[:space:]]*"\([0-9.]*\)".*/\1/p' \
          hmailserver/source/Server/Common/Application/Version.h | head -n 1)
name="hmailserver-$version-1.$disttag.$arch.rpm"
if [ ! -f "$build_dir/$name" ]; then
  ls "$build_dir"/*.rpm 2> /dev/null
  echo "PACKAGING FAILED: CPack made no $name (the name docs/PackagesRpm.md promises)."
  exit 4
fi
# The weak dependencies CMakeLists.txt sets: Suggests (7-Zip for the backup,
# python3 for hmctl) and Recommends (glibc's character-set converters, which the
# package requires only on Fedora and EL). CPack leaves every weak tag out when
# its own test decides the rpmbuild cannot take them, and the CMake 3.31 of UBI 9,
# UBI 10 and Leap 16.0 decided so on 26 September 2026 ("SUGGESTS not supported
# in provided rpmbuild"), although their rpm, 4.16 to 4.20, takes them; Fedora
# 43's CMake 3.31 with rpm 6, Fedora 44's CMake 4.3 and Tumbleweed's CMake 4.4
# kept them. Where CPack left one out, the tag goes into the spec's preamble as a
# further line of CPACK_RPM_SPEC_MORE_DEFINE, and CPack makes the package again
# from the same build; both are checked below either way. The lines are given to
# cpack itself (its -D overrides CPackConfig.cmake), with real newlines: CMake
# 3.31's CPackRPM writes the value into the spec as it is, so a CMake list's ";"
# became "Unknown tag: ;Suggests" on UBI 9, and a newline does not go through
# CMake's cache.
suggests=$(sed -n 's/^set(CPACK_RPM_PACKAGE_SUGGESTS "\(.*\)")$/\1/p' "$build_dir/CPackConfig.cmake" | head -n 1)
[ -n "$suggests" ] || { echo "PACKAGING FAILED: CPackConfig.cmake sets no CPACK_RPM_PACKAGE_SUGGESTS to check the package against"; exit 4; }
recommends=$(sed -n 's/^set(CPACK_RPM_PACKAGE_RECOMMENDS "\(.*\)")$/\1/p' "$build_dir/CPackConfig.cmake" | head -n 1)
[ -n "$recommends" ] || { echo "PACKAGING FAILED: CPackConfig.cmake sets no CPACK_RPM_PACKAGE_RECOMMENDS to check the package against"; exit 4; }
# What every later cpack of this build is given (the generic copy below too).
cpack_args=()
preamble=""
if [ -z "$(rpm -qp --suggests "$build_dir/$name")" ]; then
  echo "CPack left out Suggests: $suggests - written into the spec's preamble instead"
  preamble+=$'\n'"Suggests: $suggests"
fi
if [ -z "$(rpm -qp --recommends "$build_dir/$name")" ]; then
  echo "CPack left out Recommends: $recommends - written into the spec's preamble instead"
  preamble+=$'\n'"Recommends: $recommends"
fi
if [ -n "$preamble" ]; then
  cpack_args=(-D "CPACK_RPM_SPEC_MORE_DEFINE=$more_define$preamble")
  rm -f "$build_dir/$name"
  make_package "${cpack_args[@]}" \
    || { tail -n 40 "$out/package.log"; echo "PACKAGING FAILED: the package with its weak dependencies"; exit 4; }
fi
# One name per line: the list is split at its spaces on purpose. Recommends is one
# rich dependency, compared whole.
# shellcheck disable=SC2086
wanted_suggests=$(printf '%s\n' $suggests | sort | tr '\n' ' ')
check_weak_dependencies() {
  local got
  got=$(rpm -qp --suggests "$1" | sort | tr '\n' ' ')
  [ "$got" = "$wanted_suggests" ] \
    || { echo "PACKAGING FAILED: $(basename "$1") suggests '$got', and CMakeLists.txt '$wanted_suggests'"; exit 4; }
  got=$(rpm -qp --recommends "$1")
  [ "$got" = "$recommends" ] \
    || { echo "PACKAGING FAILED: $(basename "$1") recommends '$got', and CMakeLists.txt '$recommends'"; exit 4; }
}
check_weak_dependencies "$build_dir/$name"
# And the licences it goes out under, the tree's, each marked %license: the AGPL-3.0
# text, QuickJS's and /usr/share/licenses/hmailserver/copyright.
bash build/ci/packages/check-licenses.sh "$build_dir/$name" \
  || { echo "PACKAGING FAILED: the package's licence files (above)"; exit 4; }
cp "$build_dir/$name" "$out/"
rpm -qp --queryformat '%{NAME} %{VERSION}-%{RELEASE} %{ARCH}\n' "$out/$name"
{
  echo "# Requires"
  rpm -qp --requires "$out/$name"
  echo "# Requires(post)"
  rpm -qp --qf '[%{REQUIRENAME} %{REQUIREFLAGS:deptype}\n]' "$out/$name" | grep -i post || true
  echo "# Suggests"
  rpm -qp --suggests "$out/$name"
  echo "# Recommends"
  rpm -qp --recommends "$out/$name"
} > "$out/requires.txt"
grep -vE '^(#|rpmlib\()' "$out/requires.txt" | sed 's/^/  /'
# The binary's architecture and hardening, read from the package itself.
payload=$(mktemp -d)
( cd "$payload" && rpm2cpio "$out/$name" | cpio -idm --quiet ./usr/bin/hmailserver 2> /dev/null )
packaged="$payload/usr/bin/hmailserver"
kind=$(file -b "$packaged")
echo "/usr/bin/hmailserver: $kind"
case "$arch:$kind" in
  aarch64:*'ARM aarch64'*|x86_64:*x86-64*) ;;
  *) echo "FAILED: the packaged binary is not $arch"; exit 4 ;;
esac
case "$kind" in
  *'pie executable'*) ;;
  *) echo "FAILED: the packaged binary is not a position-independent executable"; exit 4 ;;
esac
# llvm-readelf (the llvm package) and binutils' readelf read either
# architecture's ELF.
readelf_tool=$(command -v llvm-readelf || command -v readelf) || { echo "FAILED: no readelf to check the binary with"; exit 4; }
segments=$("$readelf_tool" -l "$packaged")
dynamic=$("$readelf_tool" -d "$packaged")
case "$segments" in *GNU_RELRO*) ;; *) echo "FAILED: the packaged binary has no RELRO segment"; exit 4 ;; esac
case "$dynamic" in *BIND_NOW*|*'FLAGS_1'*NOW*) ;; *) echo "FAILED: the packaged binary is not linked with -z now"; exit 4 ;; esac
echo "PIE, with full RELRO (GNU_RELRO and BIND_NOW)"
rm -rf "$payload"

# The generic aarch64 name the update checker and generate-update-feed.py look
# for, hmailserver-<v>-1.aarch64.rpm, from the same build: only CPack's release
# field differs, so CPack is re-run, not the build. Fedora 44 aarch64 alone makes
# it, the aarch64 twin of the generic x86_64 package, which stays linux-build's
# and is install-tested on Fedora 44 (docs/PackagesRpm.md, "The generic names").
if [ "$target" = fedora44 ] && [ "$arch" = aarch64 ]; then
  generic="hmailserver-$version-1.$arch.rpm"
  { cmake -DCPACK_RPM_PACKAGE_RELEASE=1 "$build_dir" >> "$out/package.log" 2>&1 && make_package "${cpack_args[@]}"; } \
    || { tail -n 20 "$out/package.log"; echo "PACKAGING FAILED: the generic-name copy"; exit 4; }
  [ -f "$build_dir/$generic" ] || { echo "PACKAGING FAILED: CPack made no $generic"; exit 4; }
  check_weak_dependencies "$build_dir/$generic"
  bash build/ci/packages/check-licenses.sh "$build_dir/$generic" \
    || { echo "PACKAGING FAILED: the generic copy's licence files (above)"; exit 4; }
  mkdir -p "$out/generic"
  cp "$build_dir/$generic" "$out/generic/"
  echo "and generic/$generic"
fi

( cd "$out" && sha256sum -- *.rpm generic/*.rpm 2> /dev/null ) > "$out/SHA256SUMS.txt"
{
  echo "target $target ($PRETTY_NAME)"
  echo "arch $arch"
  echo "compiler $(clang --version | head -n 1)"
  echo "boost $([ "$boost" = source ] && echo "$BOOST_VERSION from source" || echo "the distribution's")"
  echo "commit ${CI_COMMIT_SHA:-unknown}"
  echo "seconds $(( $(date +%s) - started_all ))"
} > "$out/build-info.txt"
cat "$out/SHA256SUMS.txt"

# What the package was built against (build/asset-sbom.py --rpm-list): the rpm
# database of the aarch64 sysroot dnf installed, or of this container for a native
# build; openSUSE's aarch64 sysroot was listed as it was unpacked, above. And the
# Boost header the build compiled against: boost.org's, built here, or the
# distribution's.
if [ "$arch" = x86_64 ]; then
  rpm -qa --qf "$rpm_format" > "$facts/rpm.txt"
elif [ "$family" = dnf ]; then
  rpm --root "$sysroot" -qa --qf "$rpm_format" > "$facts/rpm.txt"
fi
[ -s "$facts/rpm.txt" ] || { echo "FAILED: rpm listed nothing the package was built against, for its SBOM"; exit 4; }
for header in "$sysroot/opt/boost/include/boost/version.hpp" "$sysroot/usr/include/boost/version.hpp"; do
  if [ -f "$header" ]; then
    cp "$header" "$facts/boost-version.hpp"
    break
  fi
done
[ -f "$facts/boost-version.hpp" ] || { echo "FAILED: no Boost header to record the version the build compiled against"; exit 4; }
echo "built against: $(cut -d' ' -f1 "$facts/rpm.txt" | sort -u | wc -l) package(s) recorded in built-against/"
rm -rf "$build_dir" "$sysroot"

echo ""
if [ -n "$failed" ]; then
  echo "FAILED:$failed"
  exit 1
fi
echo "BUILT: $name for $PRETTY_NAME in $(( $(date +%s) - started_all )) s."
