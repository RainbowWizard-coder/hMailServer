#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-build job of .gitlab-ci.yml: the server core with clang, the tester
# programs the CMake build makes beside it, then the programs themselves. And the
# linux-build-gcc job of .gitlab/ci/gcc.yml, the same with GCC (HM_COMPILER=gcc).
# Run in the image of build/ci/linux.Dockerfile, from the root of a checkout; by
# hand it is the same:
#
#   docker run --rm -v "$PWD":/src -w /src hmailserver-ci/linux:<tag> bash build/ci/linux-build.sh
#
# What it leaves in the checkout, for the job's artifacts (and nothing else):
#   linux-build/hmailserver     the server binary the linux-suite job tests
#   linux-build/database-claim-tests
#   linux-build/database-transaction-tests
#                               the two tester programs that need a database, which
#                               the jobs that lay one out run against it
#                               (build/ci/linux-suite.sh, build/lab/db-suite.sh)
#   linux-build/build-info.txt  the server's SHA-256 and what its --version says
#   linux-build/build.log       the whole ninja output
#   linux-build/packages/       the .deb and the .rpm CPack made from that build,
#                               each with its own SBOM beside it (<package>.cdx.json,
#                               build/asset-sbom.py), and the packages' SHA256SUMS.txt
#                               (none of it when packaging failed)
#   linux-build/package.log     the stamp check and CPack's output
#
# The build tree itself is outside the checkout (HM_BUILD_DIR, /tmp/hm-build by
# default), in the job container's own filesystem, so it goes when the container
# does and leaves nothing on the runner's disk. The environment it reads:
#   HM_BUILD_DIR    where CMake configures and ninja builds (/tmp/hm-build)
#   HM_BUILD_JOBS   ninja's -j (6: the runner gives a job six CPUs)
#   HM_OUT_DIR      where the artifacts go (linux-build)
#   HM_SOURCE_TARBALL  build from the source tarball instead of the checkout
#                   (build/ci/source-tarball.sh; hmailserver/docs/SourceTarball.md):
#                   the tarball, or a directory holding exactly one - the
#                   source-tarball job's artifact, source-tarball/. It is unpacked
#                   into HM_SOURCE_DIR (/tmp/hm-source), checked with the checker it
#                   carries, and everything below runs there - the build, the
#                   testers, the OpenPGP fixtures, the version stamps, CPack and the
#                   packages' SBOMs - so no file of the checkout is read. HM_OUT_DIR
#                   is still where the script was started. Unset, the checkout is
#                   built, as it always was.
#   HM_COMPILER     clang (the default: what the release's packages are built with)
#                   or gcc, the distribution's default GCC, which a distribution's
#                   own package of the server is built with (linux-build-gcc,
#                   .gitlab/ci/gcc.yml). A GCC build is a check of the source and
#                   makes no packages: the packages are the clang build's alone.
#
# Its exit status: 0 when the server built, every tester program passed and - for
# clang - the packages were made; 1 when a tester program or the OpenPGP round trip
# failed (the packaging's outcome is said on the line after); 2 for a setting, a
# directory, a compiler or a source tarball it cannot use; 3 when CMake's
# configuration or the build failed; 4 when everything passed
# but the packaging, so that the job fails and its log ends saying so. 4 is a code
# of its own so that a reader can tell it from a tester's failure; linux-build
# takes no allow_failure for it, since a release must never be cut without its
# packages (build/check-release-chain.py refuses an allowed exit code on a job the
# release waits for unless the code can never come from a tag's pipeline).
set -uo pipefail

# The packages record modes the umask chooses. CPack's .deb writer copies the mode
# of each file it wrote - debian-binary, control.tar.gz, data.tar.gz - into that
# member's ar header, so under umask 0002 the three were rw-rw-r-- and the .deb
# differed from one made under 0022 although every member was the same bytes
# (build/ci/linux-repro.sh found it on 26 September 2026, and a one-file CPack
# project in the CI image showed it). The packages are made under 0022, whoever
# runs this; docs/ReproducibleBuilds.md.
umask 0022

case "${HM_COMPILER:-clang}" in
  clang) compiler=clang cc=clang cxx=clang++ ;;
  gcc) compiler=gcc cc=gcc cxx=g++ ;;
  *) echo "HM_COMPILER is '${HM_COMPILER}': clang or gcc"; exit 2 ;;
esac

build_dir="${HM_BUILD_DIR:-/tmp/hm-build}"
jobs="${HM_BUILD_JOBS:-6}"
out="${HM_OUT_DIR:-linux-build}"

source_tarball="${HM_SOURCE_TARBALL:-}"
if [ -n "$source_tarball" ]; then
  case "$out" in /*) ;; *) out="$PWD/$out" ;; esac
  source_root=$(bash "$(dirname "${BASH_SOURCE[0]}")/source-tarball.sh" unpack "$source_tarball" "${HM_SOURCE_DIR:-/tmp/hm-source}") \
    || exit 2
  cd "$source_root" || exit 2
  echo "building from the source tarball, unpacked at $source_root"
fi

if [ ! -f hmailserver/source/Server/CMakeLists.txt ]; then
  echo "Run this from the root of a checkout (no hmailserver/source/Server/CMakeLists.txt here)."
  exit 2
fi

rm -rf "$out"
mkdir -p "$out"

# The clock every tool that would stamp one stamps instead, as the release build
# does: the commit's own time. In the job it comes from GitLab (git is not in the
# image); by hand, from git when there is one, and the build is simply not pinned
# when neither is there. A source tarball carries the commit's time on every file.
if [ -n "${CI_COMMIT_TIMESTAMP:-}" ]; then
  SOURCE_DATE_EPOCH=$(date -d "$CI_COMMIT_TIMESTAMP" +%s)
  export SOURCE_DATE_EPOCH
elif [ -n "$source_tarball" ]; then
  SOURCE_DATE_EPOCH=$(stat -c %Y hmailserver/source/Server/CMakeLists.txt)
  export SOURCE_DATE_EPOCH
elif command -v git > /dev/null 2>&1 && git rev-parse --git-dir > /dev/null 2>&1; then
  SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
  export SOURCE_DATE_EPOCH
fi
echo "SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-unset}"

# Every file of the tree takes the commit's time, in a pipeline. CMake's install keeps
# a copied file's own time, the .deb writer stamps SOURCE_DATE_EPOCH on every entry, but
# rpmbuild only clamps a time LATER than it (clamp_mtime_to_source_date_epoch): a file
# the checkout wrote before the commit's time went into the .rpm with that older time.
# A runner's working copy does that - a fetch leaves the files a new commit did not
# change as an earlier pipeline checked them out - so the .rpm depended on the runner's
# history: linux-repro found 610 of its 623 entries' times different in pipeline
# 2890604259 (28 September 2026), the first build from a working copy last checked out
# before the commit was made, the second from a fresh copy. Only in a pipeline: by hand,
# a build directory kept between builds would not see an edit made after the commit.
if [ -n "${CI:-}" ] && [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
  find . -path ./.git -prune -o -path "./$out" -prune -o \( -type f -o -type l \) -print0 \
    | xargs -0 -r touch -h -d "@$SOURCE_DATE_EPOCH"
  echo "every file of the tree stamped with SOURCE_DATE_EPOCH"
fi

export CC="$cc" CXX="$cxx"
if ! command -v "$CXX" > /dev/null 2>&1; then
  echo "No $CXX in this image: HM_COMPILER=$compiler needs it (build/ci/linux.Dockerfile)."
  exit 2
fi
"$CXX" --version | head -n 1
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
  # No `| head`: it stops reading, and grep or sort, still writing a long list, then
  # dies of SIGPIPE, which pipefail makes the status (58710c9d6). grep -m stops
  # reading the log itself, and sed reads its input to the end.
  grep -m 40 -E '^FAILED:' "$out/build.log"
  grep -E 'error:' "$out/build.log" | sort -u | sed -n '1,80p'
  echo "BUILD FAILED - the whole log is the job's artifact linux-build/build.log"
  exit 3
fi

server="$build_dir/hmailserver"
cp "$server" "$out/hmailserver"
# The two testers that need a database, beside the server in the artifact: the jobs
# that lay a database out run them against theirs before the server starts
# (build/ci/linux-suite.sh, build/lab/db-suite.sh). Below they are run as every
# tester is, with no database, and say they are skipped.
cp "$build_dir/database-claim-tests" "$out/database-claim-tests"
cp "$build_dir/database-transaction-tests" "$out/database-transaction-tests"
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
# network but the loopback, which network-deadline-tests and http-server-tests
# connect over to peers of their own; database-claim-tests and
# database-transaction-tests, the two that need a database, are given none here and
# say they are skipped.
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
  # starts a new one. The test lab's first build on linix found the same, as root
  # and as an ordinary user (docs/TestLab.md).
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
# nothing looks for, so none is made. The packaging runs after the testers, so
# by then they have all run; a packaging failure is reported here, and the
# script then exits 4 (above), its last line saying so - or 1 when a tester
# failed too; the jobs that need the packages fail on their absence and point
# at package.log. Until 2 October 2026 it exited 0, and linux-build was
# green with no packages in it.
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
  # And each carries the licences it goes out under, the tree's own: the AGPL-3.0
  # text, QuickJS's, and the copyright file where its format keeps it. A package
  # without them is not one the release may carry.
  for file in "$out"/packages/*.deb "$out"/packages/*.rpm; do
    bash build/ci/packages/check-licenses.sh "$file"
  done
  # Each package's own software bill of materials, beside it and made here, where
  # dpkg knows what the binary was built against: the libraries it needs and the ones
  # linked into it (build/native-dependencies.ps1), the version of each development
  # package the build used, and what the package declares it depends on. A package
  # whose SBOM cannot be made is no package: the release carries none without one.
  # From a source tarball, the Linux packages' components alone: the tarball
  # carries none of the Windows build's inputs the whole list is read from.
  local linux_only=()
  if [ -n "$source_tarball" ]; then
    linux_only=(-LinuxOnly)
  fi
  pwsh -NoProfile -File build/native-dependencies.ps1 "${linux_only[@]}" -OutputFile "$build_dir/native-dependencies.json" > /dev/null
  for file in "$out"/packages/*.deb "$out"/packages/*.rpm; do
    case "$file" in *.deb) kind=deb ;; *) kind=rpm ;; esac
    python3 build/asset-sbom.py make --kind "$kind" --asset "$file" \
      --dependencies "$build_dir/native-dependencies.json" --query-dpkg --read-package
  done
}
packaging_failed=""
if [ "$compiler" != clang ]; then
  # The packages a release carries are the clang build's; a GCC build checks the
  # source and makes none, so nothing can mistake one for the other.
  echo "none: a $compiler build makes no packages (HM_COMPILER; the packages are linux-build's)"
  code=skipped
else
  ( package ) > "$out/package.log" 2>&1
  code=$?
fi
if [ "$code" = skipped ]; then
  :
elif [ "$code" -eq 0 ]; then
  grep -E '^(Package|Version|Architecture|Depends|hmailserver |LICENCES PASSED)|agree with Version.h|\.cdx\.json: ' "$out/package.log"
  sums=$(cd "$out/packages" && sha256sum -- *.deb *.rpm)
  echo "$sums" > "$out/packages/SHA256SUMS.txt"
  echo "$sums"
else
  tail -n 30 "$out/package.log"
  rm -rf "$out/packages"
  echo "PACKAGING FAILED (exit $code): no packages; linux-build/package.log has the whole of it."
  packaging_failed=$code
fi

rm -rf "$build_dir"

echo ""
if [ -n "$failed" ]; then
  echo "FAILED:$failed"
  if [ -n "$packaging_failed" ]; then
    echo "PACKAGING FAILED (exit $packaging_failed) as well: no packages; linux-build/package.log has the whole of it."
  fi
  exit 1
fi
if [ -n "$packaging_failed" ]; then
  echo "The server built with $compiler and every tester program passed, but PACKAGING FAILED (exit $packaging_failed):"
  echo "no packages; linux-build/package.log has the whole of it."
  exit 4
fi
echo "The server built with $compiler, and every tester program passed."
