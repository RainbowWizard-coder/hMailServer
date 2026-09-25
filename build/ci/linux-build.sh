#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-build job of .gitlab-ci.yml: the server core with clang, the tester
# programs the CMake build makes beside it, then the programs themselves. Run in
# the image of build/ci/linux.Dockerfile, from the root of a checkout; by hand it
# is the same:
#
#   docker run --rm -v "$PWD":/src -w /src hmailserver-ci/linux:<tag> bash build/ci/linux-build.sh
#
# What it leaves in the checkout, for the job's artifacts (and nothing else):
#   linux-build/hmailserver     the server binary the linux-suite job tests
#   linux-build/build-info.txt  its SHA-256 and what --version says
#   linux-build/build.log       the whole ninja output
#   linux-build/packages/       the .deb and the .rpm CPack made from that build,
#                               and their SHA256SUMS.txt (none when packaging failed)
#   linux-build/package.log     the stamp check and CPack's output
#
# The build tree itself is outside the checkout (HM_BUILD_DIR, /tmp/hm-build by
# default), in the job container's own filesystem, so it goes when the container
# does and leaves nothing on the runner's disk. The environment it reads:
#   HM_BUILD_DIR    where CMake configures and ninja builds (/tmp/hm-build)
#   HM_BUILD_JOBS   ninja's -j (6: the runner gives a job six CPUs)
#   HM_OUT_DIR      where the artifacts go (linux-build)
set -uo pipefail

build_dir="${HM_BUILD_DIR:-/tmp/hm-build}"
jobs="${HM_BUILD_JOBS:-6}"
out="${HM_OUT_DIR:-linux-build}"

if [ ! -f hmailserver/source/Server/CMakeLists.txt ]; then
  echo "Run this from the root of a checkout (no hmailserver/source/Server/CMakeLists.txt here)."
  exit 2
fi

rm -rf "$out"
mkdir -p "$out"

# The clock every tool that would stamp one stamps instead, as the release build
# does: the commit's own time. In the job it comes from GitLab (git is not in the
# image); by hand, from git when there is one, and the build is simply not pinned
# when neither is there.
if [ -n "${CI_COMMIT_TIMESTAMP:-}" ]; then
  SOURCE_DATE_EPOCH=$(date -d "$CI_COMMIT_TIMESTAMP" +%s)
  export SOURCE_DATE_EPOCH
elif command -v git > /dev/null 2>&1 && git rev-parse --git-dir > /dev/null 2>&1; then
  SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
  export SOURCE_DATE_EPOCH
fi
echo "SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-unset}"

export CC=clang CXX=clang++
clang --version | head -n 1
cmake --version | head -n 1

echo ""
echo "== configure"
rm -rf "$build_dir"
if ! cmake -S hmailserver/source/Server -B "$build_dir" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr > "$out/configure.log" 2>&1; then
  cat "$out/configure.log"
  echo "CONFIGURE FAILED"
  exit 3
fi
tail -n 5 "$out/configure.log"

echo ""
echo "== build (ninja -k 0 -j $jobs)"
started=$(date +%s)
ninja -C "$build_dir" -k 0 -j "$jobs" > "$out/build.log" 2>&1
build=$?
echo "ninja exit $build after $(( $(date +%s) - started )) s; $(grep -c 'warning:' "$out/build.log" || true) warning line(s)"
if [ "$build" -ne 0 ]; then
  # -k 0 built everything that could be built, so this is every failure, not the first.
  grep -E '^FAILED:' "$out/build.log" | head -n 40
  grep -E 'error:' "$out/build.log" | sort -u | head -n 80
  echo "BUILD FAILED - the whole log is the job's artifact linux-build/build.log"
  exit 3
fi

server="$build_dir/hmailserver"
cp "$server" "$out/hmailserver"
{
  echo "sha256 $(sha256sum "$server" | cut -d' ' -f1)"
  echo "version $("$server" --version 2>&1 | head -n 1)"
  echo "commit ${CI_COMMIT_SHA:-unknown}"
} | tee "$out/build-info.txt"

failed=""

echo ""
echo "== the service answers for itself"
if "$server" --version > /dev/null && "$server" --help > /dev/null; then
  echo "--version and --help answer"
else
  failed="$failed hmailserver--version/--help"
fi

# Every tester program the CMake build makes (the *-tests targets: spf-tests,
# script-binding-tests, and any added later), found rather than listed, so a new
# one runs here the day it is added. They need no server, no database and no
# network.
testers=$(find "$build_dir" -maxdepth 1 -type f -name '*-tests' -perm -u+x | sort)
if [ -z "$testers" ]; then
  echo "No *-tests program in $build_dir: the CMake build no longer makes the testers it did."
  failed="$failed no-testers"
fi
for program in $testers; do
  name=$(basename "$program")
  echo ""
  echo "== $name"
  ( cd "$build_dir" && timeout 600 "$program" ) > "$out/$name.log" 2>&1
  code=$?
  tail -n 30 "$out/$name.log"
  echo "$name exit $code"
  [ "$code" -eq 0 ] || failed="$failed $name"
done

# The one OpenPGP message this server writes, handed to an implementation that
# is not this one: gpg decrypts what openpgp-writer sealed to the fixture keys
# GnuPG made (build/fixtures/openpgp), and its plaintext must be the input byte
# for byte. docs/ZeroAccess.md, "Proved against an implementation that is not
# this one".
echo ""
echo "== what the OpenPGP writer seals, GnuPG opens"
# Run in a subshell of its own and NOT as an if condition: bash ignores set -e
# inside a condition, subshells included, so every step would run on regardless.
openpgp() {
  set -e
  local writer="$build_dir/openpgp-writer" fixtures=build/fixtures/openpgp home
  home="$(mktemp -d)"
  chmod 700 "$home"
  export GNUPGHOME="$home"
  local G="gpg --batch --yes --no-tty --pinentry-mode loopback --passphrase openpgp-fixture-pass"
  $G --import "$fixtures/alice.sec.asc" "$fixtures/bob.sec.asc"
  # The agent that imported the keys refuses their passphrase afterwards: GnuPG
  # 2.4.8 as Ubuntu 26.04 ships it answers "Bad passphrase" to the very next
  # --decrypt, and a fresh agent takes the same passphrase (found running this
  # script in the CI image on linix, 25 September 2026). So the next command
  # starts a new one.
  gpgconf --kill gpg-agent
  for who in alice bob; do
    "$writer" "$fixtures/$who.pub.asc" "$fixtures/body.bin" > "$home/to-$who.asc"
    $G --decrypt "$home/to-$who.asc" > "$home/$who.out"
    cmp "$home/$who.out" "$fixtures/body.bin"
    echo "$who: gpg's plaintext is byte for byte the input"
  done
  "$writer" "$fixtures/alice.pub.asc" "$fixtures/pgpmime-signed.eml" --mime > "$home/sealed.eml"
  grep -q '^X-hMailServer-Zero-Access: sealed on delivery' "$home/sealed.eml"
  # Not "! grep": a negated command never trips set -e, so that line could not fail.
  if grep -q 'This is an OpenPGP/MIME signed message' "$home/sealed.eml"; then
    echo "the signed original's own preamble is outside the encrypted part"
    return 1
  fi
  sed -n '/-----BEGIN PGP MESSAGE-----/,/-----END PGP MESSAGE-----/p' "$home/sealed.eml" > "$home/inner.asc"
  $G --decrypt "$home/inner.asc" > "$home/inner.out"
  cmp "$home/inner.out" "$fixtures/pgpmime-signed.eml"
  echo "PGP/MIME: the whole original message is inside, byte for byte, and the outer headers are kept"
  gpgconf --kill all > /dev/null 2>&1 || true
  rm -rf "$home"
}
( openpgp )
code=$?
if [ "$code" -ne 0 ]; then
  echo "the OpenPGP round trip failed (exit $code)"
  failed="$failed openpgp-writer"
fi

# The .deb and the .rpm, made by CPack from the build above, for the jobs that
# install them in clean distribution containers (linux-package-deb,
# linux-package-rpm) and build the container image from the .deb (linux-image).
# The version stamps are checked first, as the old workflow's stamps job gated
# its packaging: a stamp that has drifted makes a good package under a name
# nothing looks for, so none is made. A packaging failure does NOT fail this job
# - the suite needs the binary, not the packages - it is reported here, and the
# jobs that need the packages fail on their absence and point at package.log.
echo ""
echo "== the packages"
package() {
  set -e
  bash build/check-linux-version-stamp.sh
  ( cd "$build_dir" && cpack -G DEB && cpack -G RPM )
  mkdir -p "$out/packages"
  cp "$build_dir"/*.deb "$build_dir"/*.rpm "$out/packages/"
  for deb in "$out"/packages/*.deb; do
    dpkg-deb -f "$deb" Package Version Architecture Depends
  done
  for rpm in "$out"/packages/*.rpm; do
    rpm -qp --queryformat '%{NAME} %{VERSION}-%{RELEASE} %{ARCH}\n' "$rpm"
  done
}
( package ) > "$out/package.log" 2>&1
code=$?
if [ "$code" -eq 0 ]; then
  grep -E '^(Package|Version|Architecture|Depends|hmailserver )|agree with Version.h' "$out/package.log"
  sums=$(cd "$out/packages" && sha256sum -- *.deb *.rpm)
  echo "$sums" > "$out/packages/SHA256SUMS.txt"
  echo "$sums"
else
  tail -n 30 "$out/package.log"
  rm -rf "$out/packages"
  echo "PACKAGING FAILED (exit $code): no packages; linux-build/package.log has the whole of it."
fi

rm -rf "$build_dir"

echo ""
if [ -n "$failed" ]; then
  echo "FAILED:$failed"
  exit 1
fi
echo "The server built, and every tester program passed."
