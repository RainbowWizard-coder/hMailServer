#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The source tarball, hmailserver-<version>.tar.gz: the tree of one commit, less
# every path build/source-exclusions.txt names (SQL Server Compact's headers and
# licence, which are not free, and what only the Windows build and its installer
# use), under one directory, hmailserver-<version>/. What the distributions build
# from, what a release is to carry beside its packages, and what linux-build and
# linux-suite build and test from (HM_SOURCE_TARBALL in build/ci/linux-build.sh).
# hmailserver/docs/SourceTarball.md has what is in it, what is not, and why.
#
#   bash build/ci/source-tarball.sh [REV]
#   bash build/ci/source-tarball.sh unpack TARBALL DEST
#
# The first makes it. Run from anywhere inside a clone. REV is the commit to make
# it from: HEAD when none is given (the source-tarball job gives $CI_COMMIT_SHA).
# The version, the exclusion list and the line endings are all that commit's, read
# from the commit and not from the working tree, so what is checked out or changed
# beside it makes no difference. It needs git and gzip (tar and coreutils only
# for its report). What it leaves (HM_OUT_DIR, source-tarball/ under the top of
# the clone by default): hmailserver-<version>.tar.gz and nothing else, for the
# job's artifacts.
#
# The second is how build/ci/linux-build.sh and linux-suite.sh take a tarball when
# HM_SOURCE_TARBALL names one, and needs no git: TARBALL is the file, or a
# directory holding exactly one hmailserver-*.tar.gz (the job's artifact
# directory). DEST is emptied, the tarball is unpacked into it and checked with
# the build/check-source-tarball.py it carries (and against CI_COMMIT_SHA, when
# that is set, so a pipeline builds its own commit's tarball and no other), and
# the one directory it made is printed on stdout; everything else goes to stderr.
#
# The same commit makes the same bytes wherever git's own tar writer and GNU gzip
# make them (git 2.50.1 for Windows and 2.53.0 with gzip 1.14 in Ubuntu 26.04 made
# one tarball, byte for byte, on 28 September 2026, and so did Debian's git 2.39.5
# and 2.47.3 - trixie's, which the source-tarball job installs - with Debian's gzip
# 1.10, 1.12 and 1.13; another gzip - busybox's, pigz - compresses differently, and
# the tar inside is still the same):
#   - git archive writes the tar: the commit's tree in git's own order (sorted by
#     name within each directory), every entry owned by uid 0 and gid 0 (named
#     root), stamped with the commit's time (its SOURCE_DATE_EPOCH, printed below),
#     and a pax header that records the commit (git get-tar-commit-id reads it);
#   - tar.umask 0022 fixes the modes: 0644 for a file, 0755 for a directory and
#     for a file git records as executable - whatever umask the maker has, and
#     whatever tar.umask a gitconfig sets;
#   - line endings are what .gitattributes asks of a checkout, on every platform:
#     core.autocrlf and core.eol are set here to what a Linux clone has, neither the
#     system's nor the user's attributes file is read, and a clone whose
#     .git/info/attributes sets any is refused, so the tarball holds exactly the
#     bytes a Linux CI job's checkout holds (CRLF for most text, as .gitattributes
#     says, LF for what a POSIX machine executes);
#   - gzip -n -9, with GZIP unset: no file name and no time in its header.
# build/check-source-tarball.py holds a tarball to all of that, and to the list.
set -euo pipefail

if [ "${1:-}" = "unpack" ]; then
  [ "$#" -eq 3 ] || { echo "usage: $0 unpack TARBALL DEST" >&2; exit 2; }
  tarball="$2"
  dest="$3"
  if [ -d "$tarball" ]; then
    shopt -s nullglob
    found=("$tarball"/hmailserver-*.tar.gz)
    shopt -u nullglob
    if [ "${#found[@]}" -ne 1 ]; then
      echo "REFUSED: $tarball holds ${#found[@]} hmailserver-*.tar.gz, not one." >&2
      exit 2
    fi
    tarball="${found[0]}"
  fi
  if [ ! -f "$tarball" ]; then
    echo "REFUSED: no source tarball at $tarball (the source-tarball job's artifact is missing)." >&2
    exit 2
  fi
  tarball="$(cd "$(dirname "$tarball")" && pwd -P)/$(basename "$tarball")"
  # DEST is emptied: never the root, and never a directory holding the one this
  # runs in (HM_SOURCE_DIR=. would take the checkout with it). Both are compared
  # as the file system resolves them - "..", "." and symbolic links - and not as
  # they are spelt: /tmp/work/../../tmp spells an ancestor of /tmp/work, and the
  # comparison of the spellings let rm -rf take the checkout with it.
  case "$dest" in /*) ;; *) dest="$PWD/$dest" ;; esac
  mkdir -p "$dest"
  dest="$(cd "$dest" && pwd -P)"
  dest="${dest%/}"
  here="$(pwd -P)"
  case "$here/" in
    "$dest"/*)
      echo "REFUSED: ${dest:-/}, which is emptied first, holds the directory this runs in ($here)." >&2
      exit 2
      ;;
  esac
  case "$tarball" in
    "$dest"/*)
      echo "REFUSED: ${dest:-/}, which is emptied first, holds the tarball ($tarball)." >&2
      exit 2
      ;;
  esac
  rm -rf "$dest"
  mkdir -p "$dest"
  tar -xzf "$tarball" -C "$dest" --no-same-owner
  shopt -s nullglob
  tops=("$dest"/*)
  shopt -u nullglob
  if [ "${#tops[@]}" -ne 1 ] || [ ! -d "${tops[0]}" ]; then
    echo "REFUSED: $tarball did not unpack to one directory." >&2
    exit 2
  fi
  top="${tops[0]}"
  check=(python3 "$top/build/check-source-tarball.py" --exclusions "$top/build/source-exclusions.txt")
  if [ -n "${CI_COMMIT_SHA:-}" ]; then
    check+=(--commit "$CI_COMMIT_SHA")
  fi
  if ! "${check[@]}" "$tarball" >&2; then
    echo "REFUSED: $tarball is not a source tarball this commit's checker accepts." >&2
    exit 2
  fi
  echo "$top"
  exit 0
fi

root="$(git rev-parse --show-toplevel)"
cd "$root"

rev="${1:-HEAD}"
if ! commit="$(git rev-parse --verify --quiet "$rev^{commit}")"; then
  echo "REFUSED: $rev is not a commit of this clone."
  exit 2
fi

for path in hmailserver/source/Server/Common/Application/Version.h build/source-exclusions.txt; do
  if ! git cat-file -e "$commit:$path" 2> /dev/null; then
    echo "REFUSED: $commit has no $path."
    exit 2
  fi
done

# cat-file, not show: the blob's own bytes, with no textconv a gitconfig could add.
version="$(git cat-file blob "$commit:hmailserver/source/Server/Common/Application/Version.h" | tr -d '\r' \
  | sed -n 's/^#define[[:space:]]*HMAILSERVER_VERSION[[:space:]]*"\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)".*/\1/p')"
if [ -z "$version" ]; then
  echo "REFUSED: no HMAILSERVER_VERSION in $commit's Common/Application/Version.h."
  exit 2
fi
# A release tag makes the release's tarball: its name must be the tag's version.
if [ -n "${CI_COMMIT_TAG:-}" ] && [[ "$CI_COMMIT_TAG" =~ ^v ]] && [ "$CI_COMMIT_TAG" != "v$version" ]; then
  echo "REFUSED: the tag is $CI_COMMIT_TAG and Version.h says $version: the tarball would be named for another release."
  exit 2
fi

# The list is the commit's, one literal path per line (its own header says the
# format); each becomes an exclude pathspec, "literal" so that no character of a
# path is read as a wildcard.
excludes=()
while IFS= read -r line || [ -n "$line" ]; do
  line="${line%$'\r'}"
  case "$line" in ''|'#'*) continue ;; esac
  excludes+=(":(exclude,literal)$line")
done < <(git cat-file blob "$commit:build/source-exclusions.txt")
if [ "${#excludes[@]}" -eq 0 ]; then
  echo "REFUSED: $commit's build/source-exclusions.txt leaves nothing out."
  exit 2
fi

# git archive reads three attributes files beside the commit's .gitattributes: the
# system's ($(prefix)/etc/gitattributes), the user's (core.attributesFile) and the
# clone's own $GIT_DIR/info/attributes. The first two are switched off below. The
# third cannot be, and it outranks the commit's: a "* -text" there made another
# tarball of the same commit (every line ending as the blob holds it; measured in
# WSL on 28 September 2026). So a clone that sets anything there makes none.
info_attributes="$(git rev-parse --git-path info/attributes)"
if [ -f "$info_attributes" ] && grep -q -v -E '^[[:space:]]*(#|$)' "$info_attributes"; then
  echo "REFUSED: $info_attributes sets attributes of this clone's own, which git archive applies over the"
  echo "commit's .gitattributes: the tarball would not be the commit's alone. Make it from a clone without them."
  exit 2
fi

epoch="$(git log -1 --format=%ct "$commit")"
name="hmailserver-$version"
out="${HM_OUT_DIR:-$root/source-tarball}"
mkdir -p "$out"
tarball="$out/$name.tar.gz"
rm -f "$tarball" "$tarball.partial"

GIT_ATTR_NOSYSTEM=1 git -c core.autocrlf=false -c core.eol=lf -c core.attributesFile=/dev/null -c tar.umask=0022 \
  archive --format=tar --prefix="$name/" "$commit" -- . "${excludes[@]}" \
  | env -u GZIP gzip -n -9 > "$tarball.partial"
mv "$tarball.partial" "$tarball"

echo "version            $version"
echo "commit             $commit"
echo "SOURCE_DATE_EPOCH  $epoch ($(date -u -d "@$epoch" +%Y-%m-%dT%H:%M:%SZ 2> /dev/null || echo "the commit's time"))"
echo "left out           ${#excludes[@]} path(s) of build/source-exclusions.txt"
echo "entries            $(gzip -dc "$tarball" | tar -tf - | wc -l | tr -d ' ')"
echo "size               $(wc -c < "$tarball" | tr -d ' ') bytes"
echo "sha256             $(sha256sum "$tarball" | cut -d' ' -f1)  $(basename "$tarball")"
