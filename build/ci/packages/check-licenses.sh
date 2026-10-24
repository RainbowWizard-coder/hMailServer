#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The licence files the project's own .deb and .rpm carry, held to the tree they
# were built from:
#
#   /usr/share/hmailserver/licenses/AGPL-3.0.txt   hMailServer's own licence: the
#                                                  repository's LICENSE without its
#                                                  carriage returns, the FSF's file
#   /usr/share/hmailserver/licenses/QuickJS.txt    the licence of QuickJS, compiled
#                                                  into the server
#   /usr/share/hmailserver/licenses/Unicode-data.txt
#                                                  the Unicode License v3 of Unicode's
#                                                  data for UTS 46, compiled into the
#                                                  server as tables: Common/Idna/ucd/
#                                                  unicode.LICENSE
#   /usr/share/hmailserver/licenses/HTML-named-character-references.txt
#                                                  the BSD 3-Clause terms of HTML's list
#                                                  of named character references,
#                                                  compiled in as a table: Common/
#                                                  AntiSpam/whatwg/entities.LICENSE
#   /usr/share/hmailserver/licenses/Feather-icons.txt
#                                                  the MIT licence of Feather, whose
#                                                  icons the webmail's page, compiled
#                                                  in, is drawn after: docs/Licenses/
#                                                  License - Feather icons.txt
#   the copyright file                             platform/packaging/debian-src/debian/copyright:
#     .deb  /usr/share/doc/hmailserver/copyright   (Debian Policy 12.5)
#     .rpm  /usr/share/licenses/hmailserver/copyright
#
# each 0644 and root's, each the package's own, and in the .rpm all six marked
# %license.
# CMakeLists.txt installs the first five; CPack puts the copyright file in
# (platform/packaging/cpack-licenses.cmake).
#
#   bash build/ci/packages/check-licenses.sh PACKAGE.deb | PACKAGE.rpm
#       what the package file holds, read from it without installing it: a .deb
#       unpacked with dpkg-deb, its modes and owners from its own listing; an
#       .rpm from its header, which gives each file's SHA-256, mode, owner and
#       flags
#   bash build/ci/packages/check-licenses.sh --installed deb | rpm
#       what the installed package left on this system - which is where a
#       container image's dpkg path-exclude or dnf's tsflags=nodocs drops a file
#       the package holds but did not mark to keep - and that dpkg -L or rpm -qL
#       names each as the package's
#
# Run from the root of a checkout or of the source tarball: every file is
# compared with the tree's by SHA-256 - QuickJS.txt and Feather-icons.txt with
# their carriage returns taken off both sides, since each is installed as the
# checkout holds it, and the
# two data sets' notices exactly, being kept byte for byte (.gitattributes marks
# their directories -text). It needs
# bash, coreutils, grep and dpkg or rpm, which every image the package jobs run
# in has. Exit 0 when every file is there and right, 1 when one is not,
# 2 on a usage error.
set -uo pipefail

usage() {
  echo "usage: $0 PACKAGE.deb|PACKAGE.rpm"
  echo "       $0 --installed deb|rpm"
  exit 2
}

case "${1:-}" in
  --installed)
    mode=installed
    kind="${2:-}"
    case "$kind" in deb|rpm) ;; *) usage ;; esac
    ;;
  *.deb|*.rpm)
    mode=package
    package="$1"
    kind="${package##*.}"
    [ -f "$package" ] || { echo "REFUSED: no package file $package."; exit 2; }
    ;;
  *) usage ;;
esac

server=hmailserver/source/Server
agpl_source=LICENSE
quickjs_source=$server/quickjs/LICENSE
unicode_source=$server/Common/Idna/ucd/unicode.LICENSE
whatwg_source=$server/Common/AntiSpam/whatwg/entities.LICENSE
feather_source="hmailserver/docs/Licenses/License - Feather icons.txt"
copyright_source=$server/platform/packaging/debian-src/debian/copyright
for f in "$agpl_source" "$quickjs_source" "$unicode_source" "$whatwg_source" "$feather_source" "$copyright_source"; do
  [ -f "$f" ] || { echo "REFUSED: no $f here. Run this from the root of a checkout or of the source tarball."; exit 2; }
done

sha() { sha256sum | cut -d' ' -f1; }
licenses=/usr/share/hmailserver/licenses
if [ "$kind" = deb ]; then
  copyright=/usr/share/doc/hmailserver/copyright
else
  copyright=/usr/share/licenses/hmailserver/copyright
fi
# path, how it is compared (exact, or text: carriage returns off both sides), the
# SHA-256 it must have, and for a text file the SHA-256 of its bytes as the tree
# holds them, which are right as well and are what an .rpm's header can be held to
expected="$licenses/AGPL-3.0.txt exact $(tr -d '\r' < "$agpl_source" | sha) -
$licenses/QuickJS.txt text $(tr -d '\r' < "$quickjs_source" | sha) $(sha < "$quickjs_source")
$licenses/Unicode-data.txt exact $(sha < "$unicode_source") -
$licenses/HTML-named-character-references.txt exact $(sha < "$whatwg_source") -
$licenses/Feather-icons.txt text $(tr -d '\r' < "$feather_source" | sha) $(sha < "$feather_source")
$copyright exact $(sha < "$copyright_source") -"

where="installed"
[ "$mode" = package ] && where="in $(basename "$package")"
echo "the licence files $where, against the tree's:"

failed=0
bad() {
  echo "  $1"
  failed=$((failed + 1))
}

root=""
cleanup() {
  if [ -n "$root" ]; then rm -rf "$root"; fi
}
trap cleanup EXIT
listing=""
if [ "$mode" = package ] && [ "$kind" = deb ]; then
  root=$(mktemp -d)
  dpkg-deb -x "$package" "$root" || { echo "FAILED: dpkg-deb could not unpack $package"; exit 1; }
  # The modes and owners as the package records them: an unpacked copy takes the
  # umask and the user of whoever unpacked it.
  listing=$(dpkg-deb -c "$package") || { echo "FAILED: dpkg-deb could not list $package"; exit 1; }
elif [ "$mode" = package ]; then
  algo=$(rpm -qp --qf '%{FILEDIGESTALGO}' "$package" 2> /dev/null)
  # 8 is SHA-256 (rpm takes RFC 4880's numbers), every rpmbuild's default since 4.14.
  [ "$algo" = 8 ] || { echo "FAILED: $package gives its files' digests with algorithm '$algo', not SHA-256 (8)"; exit 1; }
  listing=$(rpm -qp --qf '[%{FILENAMES}\t%{FILEDIGESTS}\t%{FILEMODES:perms}\t%{FILEUSERNAME}:%{FILEGROUPNAME}\t%{FILEFLAGS:fflags}\n]' "$package") \
    || { echo "FAILED: rpm could not read $package"; exit 1; }
elif [ "$kind" = deb ]; then
  listing=$(dpkg -L hmailserver 2>&1) || { echo "FAILED: dpkg -L hmailserver: $listing"; exit 1; }
else
  listing=$(rpm -qL hmailserver 2>&1) || { echo "FAILED: rpm -qL hmailserver: $listing"; exit 1; }
fi

while read -r path how want as_held; do
  mark=""
  if [ "$mode" = package ] && [ "$kind" = rpm ]; then
    got=""
    while IFS=$'\t' read -r name digest modes owners fflags; do
      if [ "$name" = "$path" ]; then
        got=$digest perms=$modes owner=$owners flags=$fflags
        break
      fi
    done <<< "$listing"
    [ -n "$got" ] || { bad "MISSING  $path"; continue; }
    shown=$got
    if [ "$how" = text ] && [ "$got" = "$as_held" ]; then
      got=$want
    fi
    case "$flags" in
      *l*) mark="  %license" ;;
      *) bad "WRONG    $path: not marked %license (flags '$flags')"; continue ;;
    esac
  else
    file="${root}${path}"
    [ -f "$file" ] || { bad "MISSING  $path"; continue; }
    shown=$(sha < "$file")
    if [ "$how" = text ]; then
      got=$(tr -d '\r' < "$file" | sha)
    else
      got=$shown
    fi
    if [ "$mode" = package ]; then
      perms="" owner=""
      while read -r modes owners _ _ _ name _; do
        if [ "$name" = ".$path" ]; then
          perms=$modes owner=${owners/\//:}
          break
        fi
      done <<< "$listing"
    else
      perms=$(stat -c %A "$file")
      owner=$(stat -c %U:%G "$file")
      # The package's own record: dpkg -L names the file, rpm -qL names it a licence.
      grep -Fqx -- "$path" <<< "$listing" \
        || { bad "WRONG    $path: $([ "$kind" = deb ] && echo 'dpkg -L' || echo 'rpm -qL') hmailserver does not name it"; continue; }
      [ "$kind" = rpm ] && mark="  %license"
    fi
  fi
  [ "$got" = "$want" ] || { bad "WRONG    $path: SHA-256 $shown, not the tree's"; continue; }
  [ "$perms" = "-rw-r--r--" ] || { bad "WRONG    $path: mode '$perms', not -rw-r--r--"; continue; }
  [ "$owner" = root:root ] || { bad "WRONG    $path: owner '$owner', not root:root"; continue; }
  echo "  present  $path  ${shown:0:12}$mark"
done <<< "$expected"

if [ "$failed" -ne 0 ]; then
  echo "FAILED: $failed licence file(s) $where missing or not the tree's"
  exit 1
fi
echo "LICENCES PASSED: the AGPL-3.0 text, the notices of QuickJS, Unicode's data, HTML's named character references and the Feather icons, and the copyright file $where are the tree's"
