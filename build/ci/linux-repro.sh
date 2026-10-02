#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The linux-repro job of .gitlab-ci.yml: is the Linux build reproducible? The
# server, the .deb and the .rpm that linux-build made - the very files a v* tag's
# release job ships - are made a second time from the same commit, in the same
# image, by the same script (build/ci/linux-build.sh), and compared byte for byte.
# docs/ReproducibleBuilds.md says what is claimed and what is not.
#
#   bash build/ci/linux-repro.sh
#
# Run in the image of build/ci/linux.Dockerfile, from the root of a checkout. In
# the job, linux-build's artifact (linux-build/) is the first build, and an
# artifact without the binary or the packages fails the job. By hand, with no
# linux-build/ in the checkout, this script makes the first build itself, with
# linux-build.sh as the job would, and then the second:
#
#   docker run --rm -v "$PWD":/src -w /src -e CI_COMMIT_TIMESTAMP=<the commit's %cI> \
#     hmailserver-ci/linux:<tag> bash build/ci/linux-repro.sh
#
# The second build differs from the first in everything that must not matter,
# so that a difference in the output shows the build depends on it:
#   - the source tree is a copy at another path (HM_REPRO_SOURCE, /tmp/hm-repro-source),
#     without .git and without the first build's output;
#   - its files have the modes a checkout made under umask 0027 gives them (0640, and
#     0750 for a directory or an executable), where the job's own checkout has 0666
#     and 0777 (the GitLab docker executor runs a job under umask 0000) and a clone
#     by hand usually 0644 and 0755, so a mode a package takes from the source shows,
#     as the .deb's maintainer scripts' did until 26 September 2026;
#   - the build directory is another, of another length (HM_REPRO_BUILD_DIR,
#     /tmp/hm-repro-second-build, where linux-build uses /tmp/hm-build);
#   - it is started under umask 0002 rather than the job's own (0000 in a GitLab job,
#     0022 in a plain docker run); linux-build.sh sets its own, so this shows that the
#     caller's does not reach the packages;
#   - the time zone is fourteen hours east of UTC (TZ=REPRO-14);
#   - it runs later, and in the job in another container than linux-build's.
# What it does not vary: the image (the compiler, the libraries, CMake), the
# commit, and SOURCE_DATE_EPOCH, which is the commit's own date in both.
#
# What must be identical, byte for byte, or the job fails (exit 1): the hmailserver
# binary as linux-build.sh leaves it (unstripped), the .deb and the .rpm. When a
# package differs, the report says where: for the .deb, its ar members' headers and
# each member, and inside a tar member the listing (modes, owners, times, order)
# and each file's bytes; for the .rpm, its header's build time and host, and each
# file's path, size, digest, mode, owner and time.
#
# Exit 2: not run from a checkout, or no commit date to build with; exit 3: a build
# made no binary or no packages (in a pipeline, linux-build's artifact included);
# exit 1: a difference that fails it; 0: identical.
#
# Left in HM_OUT_DIR (linux-repro/), for the job's artifacts:
#   report.txt              every digest compared and the verdict
#   image-packages.txt      every package in the image, with its version
#   first-build.log         linux-build.sh's output, when this script made the first build
#   second-build.log        linux-build.sh's output for the second build
#   second/                 the second build's build-info.txt, build.log and package.log
#   *.diff                  what differed, when anything did
# The second build's binary and packages are kept only when they differ.
set -uo pipefail

here="$(pwd)"
out="${HM_OUT_DIR:-linux-repro}"
first="${HM_REPRO_FIRST:-linux-build}"
source_b="${HM_REPRO_SOURCE:-/tmp/hm-repro-source}"
build_b="${HM_REPRO_BUILD_DIR:-/tmp/hm-repro-second-build}"

if [ ! -f hmailserver/source/Server/CMakeLists.txt ]; then
  echo "Run this from the root of a checkout (no hmailserver/source/Server/CMakeLists.txt here)."
  exit 2
fi
case "$out" in /*) ;; *) out="$here/$out" ;; esac
case "$first" in /*) ;; *) first="$here/$first" ;; esac

# Both builds stamp the commit's own date (linux-build.sh turns CI_COMMIT_TIMESTAMP
# into SOURCE_DATE_EPOCH). The second build's tree is a copy without .git, so a
# date read from git is read once, here, and handed to both.
if [ -z "${CI_COMMIT_TIMESTAMP:-}" ]; then
  if command -v git > /dev/null 2>&1 && git rev-parse --git-dir > /dev/null 2>&1; then
    CI_COMMIT_TIMESTAMP="$(git log -1 --format=%cI)"
  else
    echo "No CI_COMMIT_TIMESTAMP and no git: nothing fixes the date both builds would stamp,"
    echo "so the comparison would prove nothing. Pass -e CI_COMMIT_TIMESTAMP=<git log -1 --format=%cI>."
    exit 2
  fi
fi
export CI_COMMIT_TIMESTAMP

rm -rf "$out"
mkdir -p "$out"
report="$out/report.txt"
say() { echo "$*" | tee -a "$report"; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

say "linux-repro: is the Linux build reproducible?"
say "commit     ${CI_COMMIT_SHA:-unknown} (${CI_COMMIT_REF_NAME:-not a pipeline})"
say "image      ${CI_JOB_IMAGE:-(by hand)}"
say "date       $CI_COMMIT_TIMESTAMP (SOURCE_DATE_EPOCH $(date -d "$CI_COMMIT_TIMESTAMP" +%s))"
say "compiler   $(clang --version | head -n 1)"
say "cmake      $(cmake --version | head -n 1)"
# Every package in the image, with its version: what somebody rebuilding a release
# has to match, since the image's Ubuntu packages are the archive's of the day the
# image was built (docs/ReproducibleBuilds.md, "What is not pinned").
dpkg-query -W -f '${Package} ${Version} ${Architecture}\n' 2> /dev/null | sort > "$out/image-packages.txt"
say "packages   $(wc -l < "$out/image-packages.txt") in the image, listed in image-packages.txt"

# ------------------------------------------------------------ the first build
built_first=0
if [ -f "$first/hmailserver" ] && compgen -G "$first/packages/*.deb" > /dev/null; then
  say "first      $first: linux-build's artifact"
elif [ -n "${CI_JOB_ID:-}" ]; then
  # In a pipeline the first build is the one the release would ship, and nothing
  # else will do: building one here would compare two builds of this job's own.
  say "first      $first: linux-build's artifact has no binary or no .deb (linux-build/package.log"
  say "           says why). Nothing to compare the release's files with."
  exit 3
else
  say "first      none in $first: built now, with build/ci/linux-build.sh"
  built_first=1
  HM_OUT_DIR="$first" bash build/ci/linux-build.sh > "$out/first-build.log" 2>&1
  code=$?
  say "           linux-build.sh exit $code"
fi

# ----------------------------------------------------------- the second build
rel_out="${out#"$here"/}"
rel_first="${first#"$here"/}"
rm -rf "$source_b" "$build_b"
mkdir -p "$source_b"
tar -C "$here" --exclude=./.git --exclude="./$rel_out" --exclude="./$rel_first" \
  --exclude=./linux-build --exclude=./linux-repro -cf - . | tar -C "$source_b" -xf -
chmod -R g-w,o-rwx "$source_b"
say "second     source $source_b (modes as under umask 0027), build $build_b, umask 0002, TZ=REPRO-14"
started=$(date +%s)
( cd "$source_b" && umask 0002 && export TZ=REPRO-14 \
    && HM_BUILD_DIR="$build_b" HM_OUT_DIR="$out/second" bash build/ci/linux-build.sh ) > "$out/second-build.log" 2>&1
code=$?
say "           linux-build.sh exit $code after $(( $(date +%s) - started )) s"
if [ "$code" -ne 0 ]; then
  # linux-build gates the testers; this job answers whether the output is the same.
  say "           (a non-zero exit is linux-build's to answer for: its tester programs, the"
  say "           OpenPGP round trip or, with exit 4, its packaging; the comparison below goes"
  say "           ahead when the files exist)"
fi
rm -rf "$source_b" "$build_b"

missing=""
for f in "$first/hmailserver" "$out/second/hmailserver"; do
  [ -f "$f" ] || missing="$missing $f"
done
deb_a=$(compgen -G "$first/packages/*.deb" | head -n 1)
deb_b=$(compgen -G "$out/second/packages/*.deb" | head -n 1)
rpm_a=$(compgen -G "$first/packages/*.rpm" | head -n 1)
rpm_b=$(compgen -G "$out/second/packages/*.rpm" | head -n 1)
[ -n "$deb_a" ] || missing="$missing $first/packages/*.deb"
[ -n "$deb_b" ] || missing="$missing $out/second/packages/*.deb"
[ -n "$rpm_a" ] || missing="$missing $first/packages/*.rpm"
[ -n "$rpm_b" ] || missing="$missing $out/second/packages/*.rpm"
if [ -n "$missing" ]; then
  say ""
  say "A BUILD MADE NOTHING TO COMPARE: missing$missing"
  [ "$built_first" -eq 1 ] && tail -n 30 "$out/first-build.log"
  tail -n 30 "$out/second-build.log"
  exit 3
fi

# ------------------------------------------------------------- the comparison
failed=""
say ""
say "== the server binary (unstripped, as linux-build.sh leaves it)"
a=$(sha "$first/hmailserver"); b=$(sha "$out/second/hmailserver")
say "first      $a"
say "second     $b"
if [ "$a" = "$b" ]; then
  say "           identical"
else
  say "           DIFFERENT"
  failed="$failed binary"
  # Which sections differ says where to look (a path, a time, a build id).
  for n in first second; do
    f="$first/hmailserver"; [ "$n" = second ] && f="$out/second/hmailserver"
    section="$(mktemp)"
    for s in $(readelf -W -S "$f" | sed -n 's/^ *\[ *[0-9][0-9]*\] *//p' | awk '$1 != "NULL" { print $1 }'); do
      rm -f "$section"
      objcopy -O binary --only-section="$s" "$f" "$section" 2> /dev/null
      printf '%s %s\n' "$s" "$( [ -f "$section" ] && sha256sum "$section" | cut -c1-16 || echo none)"
    done > "$out/section-digests-$n.txt"
    rm -f "$section"
  done
  diff "$out/section-digests-first.txt" "$out/section-digests-second.txt" > "$out/binary-sections.diff"
  say "           sections that differ: $(grep -c '^<' "$out/binary-sections.diff") (binary-sections.diff)"
fi

say ""
say "== the .deb: $(basename "$deb_a")"
[ "$(basename "$deb_a")" = "$(basename "$deb_b")" ] || say "           the second is named $(basename "$deb_b")"
work="$(mktemp -d)"
mkdir -p "$work/first" "$work/second"
( cd "$work/first" && ar x "$deb_a" )
( cd "$work/second" && ar x "$deb_b" )
if [ "$(sha "$deb_a")" = "$(sha "$deb_b")" ]; then
  say "whole      identical $(sha "$deb_a")"
else
  say "whole      DIFFERENT ($(sha "$deb_a") / $(sha "$deb_b"))"
  failed="$failed deb"
fi
# The ar archive's own headers: each member's name, mode, owner, time and size.
say "ar headers, first:"
ar tvO "$deb_a" | sed 's/^/           /' | tee -a "$report"
if [ "$(ar tvO "$deb_a")" != "$(ar tvO "$deb_b")" ]; then
  say "ar headers, second (they differ):"
  ar tvO "$deb_b" | sed 's/^/           /' | tee -a "$report"
fi
data=$(ar t "$deb_a" | grep '^data\.tar' | head -n 1)
control=$(ar t "$deb_a" | grep '^control\.tar' | head -n 1)
for member in $(ar t "$deb_a"); do
  if [ -f "$work/second/$member" ]; then
    ma=$(sha "$work/first/$member"); mb=$(sha "$work/second/$member")
    if [ "$ma" = "$mb" ]; then verdict=identical; else verdict=DIFFERENT; fi
  else
    ma=$(sha "$work/first/$member"); mb="(absent)"; verdict=DIFFERENT
  fi
  say "$(printf '%-22s %s %s' "$member" "$verdict" "$ma")"
  [ "$verdict" = identical ] && continue
  # What differs inside a tar member: the listing (names, modes, owners, times,
  # order) and each file's bytes.
  case "$member" in
    *.tar*)
      for n in first second; do
        tar --numeric-owner --full-time -tvf "$work/$n/$member" > "$work/$n.list" 2>&1
        mkdir -p "$work/$n.x"
        tar -xf "$work/$n/$member" -C "$work/$n.x" 2> /dev/null
        ( cd "$work/$n.x" && find . -type f -print0 | sort -z | xargs -0 -r sha256sum ) > "$work/$n.sums"
      done
      {
        echo "## tar listing (first <, second >)"
        diff "$work/first.list" "$work/second.list"
        echo "## file contents (first <, second >)"
        diff "$work/first.sums" "$work/second.sums"
      } > "$out/deb-$member.diff"
      say "           listing lines that differ: $(diff "$work/first.list" "$work/second.list" | grep -c '^<'),"
      say "           files whose bytes differ: $(diff "$work/first.sums" "$work/second.sums" | grep -c '^<') (deb-$member.diff)"
      rm -rf "$work/first.x" "$work/second.x"
      ;;
  esac
done
[ -n "$data" ] || say "           no data.tar member"
[ -n "$control" ] || say "           no control.tar member"

say ""
say "== the .rpm: $(basename "$rpm_a")"
if [ "$(sha "$rpm_a")" = "$(sha "$rpm_b")" ]; then
  say "whole      identical $(sha "$rpm_a")"
else
  say "whole      DIFFERENT ($(sha "$rpm_a") / $(sha "$rpm_b"))"
  failed="$failed rpm"
fi
for n in first second; do
  f="$rpm_a"; [ "$n" = second ] && f="$rpm_b"
  rpm -qp --queryformat '%{BUILDTIME} %{BUILDHOST}\n' "$f" 2> /dev/null > "$work/$n.rpmhead"
  # One line a file, tab-separated (two directory names hold a space): path,
  # size, digest, mode, owner, group, time.
  rpm -qp --queryformat '[%{FILENAMES}\t%{FILESIZES}\t%{FILEDIGESTS}\t%{FILEMODES:octal}\t%{FILEUSERNAME}\t%{FILEGROUPNAME}\t%{FILEMTIMES}\n]' \
    "$f" 2> /dev/null > "$work/$n.dump"
done
say "build time and host: $(cat "$work/first.rpmhead") / $(cat "$work/second.rpmhead")"
# Each file's path, size, digest, mode and owner: the payload's contents.
if diff <(cut -f1-6 "$work/first.dump") <(cut -f1-6 "$work/second.dump") > "$out/rpm-contents.diff"; then
  say "files      identical in path, size, digest, mode and owner ($(wc -l < "$work/first.dump") entries)"
  rm -f "$out/rpm-contents.diff"
else
  say "files      $(grep -c '^<' "$out/rpm-contents.diff") entries differ in path, size, digest, mode or owner (rpm-contents.diff)"
fi
if diff <(cut -f1,7 "$work/first.dump") <(cut -f1,7 "$work/second.dump") > "$out/rpm-mtimes.diff"; then
  say "file times identical"
  rm -f "$out/rpm-mtimes.diff"
else
  say "file times: $(grep -c '^<' "$out/rpm-mtimes.diff") entries differ (rpm-mtimes.diff)"
fi
rm -rf "$work"

say ""
if [ -n "$failed" ]; then
  say "NOT REPRODUCIBLE:$failed. The second build's binary and packages are kept in second/."
  exit 1
fi
# Identical: the second build's copies add nothing to the artifact.
rm -f "$out/second/hmailserver"
rm -rf "$out/second/packages"
say "REPRODUCIBLE: the server binary, the .deb and the .rpm are byte for byte the same from"
say "another source path with other file modes, another build directory, another umask and"
say "another time zone."
