#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The source of the shared libraries each AppImage carries in usr/lib, as a release file
# of its own: hmailserver-<version>-appimage-bundled-sources.tar.gz
# (docs/PackagesAppImage.md, "Licences").
#
# Why: build-appimage.sh copies into the image every library ldd resolves for the
# server but the ones left to the host - on Ubuntu 24.04, libpq and libldap and what
# they need: Kerberos, Cyrus SASL, GnuTLS and its Nettle, GMP, libidn2, libunistring,
# libtasn1 and p11-kit, com_err, keyutils and libffi. Several are under the LGPL, which
# asks for the library's source to be offered with the binary, from the same place
# (LGPL-2.1 section 4 and 6(d); LGPL-3.0 through the GPL-3.0's section 6(d)). This file
# carries the source of every one of them, not only of those: see licence_of below.
# Each image names it in usr/share/hmailserver/licenses/bundled.txt, beside the
# packages' copyright files.
#
# The source is Ubuntu's own: each source package - its .dsc, its upstream archive(s)
# and Ubuntu's .debian.tar.* or .diff.gz - at exactly the version the image's package
# was built from, from Ubuntu's archive (https://archive.ubuntu.com/ubuntu/), or, for a
# version the archive no longer indexes, from Ubuntu's snapshot service
# (https://snapshot.ubuntu.com/ubuntu/<time>/) at a time the image's build recorded.
# Nothing is fetched from anywhere else, and no redirect is followed. Every file is held
# to its checksum along one chain: the suite's InRelease, verified with gpgv against
# Ubuntu's archive signing keys (/usr/share/keyrings/ubuntu-archive-keyring.gpg, from
# ubuntu-keyring in the image the job names); the Sources index, held to the SHA-256
# and size the InRelease gives it; the .dsc and every other file of the source package,
# held to the SHA-256 and size the Sources index gives; and every file the .dsc lists,
# held again to the SHA-256 and size the .dsc gives, the .dsc naming the package and
# version the image's record does.
#
#   bash build/ci/packages/appimage-bundled-sources.sh check DIR...
#
#     Each DIR is an AppImage job's appimage/built-against-<arch>/ (appimage-build.sh):
#     every source package its bundled.txt names is one licence_of knows. No network.
#     appimage-build.sh runs it, so that a library an image starts to carry fails its
#     own job, and is looked at, before a release carries it.
#
#   bash build/ci/packages/appimage-bundled-sources.sh pack [DIR...]
#
#     In the appimage-bundled-sources job (ubuntu:24.04, the appimage-x86_64 job's
#     image), from both AppImage jobs' records (appimage/built-against-*/ by default):
#     bundled.txt, each library with its package, version, source package and source
#     version, and archive.txt, when that job's apt read the archive, the date of each
#     index it read and when its dpkg installed each of those packages. Every source
#     package at every version the records name is
#     fetched and held as above, and packed with README.txt (each package, the libraries
#     each image took from it, its licence and what that asks, the origin and checksum of
#     every file, how to build it again as Ubuntu did and how to run an AppImage with a
#     changed library), the records as bundled-<arch>.txt, and SHA256SUMS, reproducibly:
#     sorted, every entry dated SOURCE_DATE_EPOCH (CI_COMMIT_TIMESTAMP's time when it is
#     not set), owned by 0:0, modes 0644 and 0755, GNU tar's own format, gzip -n -9.
#     HM_OUT_DIR (appimage-bundled-sources) gets the tarball and SHA256SUMS.txt naming it.
#
# Run from the root of a checkout, in ubuntu:24.04; pack needs network access to the two
# hosts above and installs curl, xz-utils and ca-certificates with apt.
set -euo pipefail

# Each source package whose libraries an AppImage may carry, with its licence as its
# Debian copyright file gives it for the files of the library the image takes, and what
# that licence asks of a binary: "source", its source offered with the binary, from the
# same place, besides the copyright file and the licence's text; "notice", the copyright
# file alone. Every one is packed all the same: the notice-only licences ask for no
# source, but the copyright files these come with give GPL terms to parts of their
# Debian packaging (libffi's debian/*, cyrus-sasl2's debian/rules and tests) or, for
# simplicity, to files that are not (e2fsprogs' catch-all GPL-2 stanza, which covers
# libcom_err's MIT-style lib/et), and whether a patch reaches a library is a judgement
# the release should not rest on; the source of all of them is some 75 MB, once for
# both images. A source package not named here fails check and pack.
licence_of() {
  case "$1" in
    gnutls28) echo "source|LGPL-2.1-or-later, with parts under LGPL-3.0-or-later OR GPL-2.0-or-later (lib/nettle's backported, GOST and FIPS code), so the library as built goes out under the LGPL-3.0-or-later (upstream's README says as much of its binaries, which link GMP); CRYPTOGAMS (BSD-style, or the GPL) for lib/accelerated/x86; the programs and libgnutls-openssl, not bundled, are GPL-3.0-or-later" ;;
    nettle) echo "source|LGPL-3.0-or-later OR GPL-2.0-or-later (libnettle and libhogweed)" ;;
    gmp) echo "source|LGPL-3.0-or-later OR GPL-2.0-or-later" ;;
    libidn2) echo "source|LGPL-3.0-or-later OR GPL-2.0-or-later (the library, lib/*.c by their own notices and upstream's README; Debian's copyright file has a stanza giving lib/* the GPL-3.0-or-later, which those notices do not bear out; its programs are GPL-3.0-or-later)" ;;
    libunistring) echo "source|LGPL-3.0-or-later OR GPL-2.0-or-later" ;;
    libtasn1-6) echo "source|LGPL-2.1-or-later (the library; its programs are GPL-3.0-or-later)" ;;
    keyutils) echo "source|LGPL-2.0-or-later (libkeyutils; its programs are GPL-2.0-or-later)" ;;
    e2fsprogs) echo "notice|MIT-style (libcom_err, lib/et: its files' own notices and upstream's NOTICE; Debian's copyright file puts lib/et under its catch-all GPL-2 stanza, which it says is a simplification)" ;;
    krb5) echo "notice|MIT and other BSD-style notices (a few files BSD-3-Clause OR GPL-2.0-or-later, at the recipient's choice)" ;;
    openldap) echo "notice|OLDAP-2.8 (GPL only for build scripts that are not in the library)" ;;
    cyrus-sasl2) echo "notice|BSD-3-Clause-Attribution (Carnegie Mellon University's, with its acknowledgement), RSA Data Security's MD5 notice (lib/md5.c) and other BSD-style licences" ;;
    postgresql-16) echo "notice|PostgreSQL" ;;
    p11-kit) echo "notice|BSD-3-Clause (LGPL-2.1-or-later only for a fuzzing harness that is not in the library)" ;;
    libffi) echo "notice|MIT (GPL only for build scripts, tests and Debian's packaging)" ;;
    *) return 1 ;;
  esac
}

ARCHIVE="https://archive.ubuntu.com/ubuntu"
SNAPSHOTS="https://snapshot.ubuntu.com/ubuntu"
KEYRING="/usr/share/keyrings/ubuntu-archive-keyring.gpg"

die() {
  echo "appimage-bundled-sources: $*" >&2
  exit 1
}

# One AppImage job's record: DIR/bundled.txt, "<soname> <package>:<arch> <version>
# <sha256> <source> <source version>" (appimage-build.sh). Checks every line, and
# writes "<source> <source version> <arch> <package>:<arch> <version> <soname>" for
# each library to standard output.
read_bundled() {
  local dir=$1 soname owner version hash source_package source_version extra
  [ -s "$dir/bundled.txt" ] || die "$dir/bundled.txt is missing or empty: the AppImage job writes it (appimage-build.sh)"
  while read -r soname owner version hash source_package source_version extra <&3; do
    [ -n "$soname" ] || continue
    [ -z "$extra" ] && [ -n "$source_version" ] || die "$dir/bundled.txt: '$soname $owner ...' is not '<soname> <package>:<arch> <version> <sha256> <source> <source version>'"
    printf '%s' "$soname" | grep -Eq '^[A-Za-z0-9._+-]+\.so(\.[0-9]+)*$' || die "$dir/bundled.txt names a library '$soname'"
    printf '%s' "$owner" | grep -Eq '^[a-z0-9][a-z0-9.+-]*:[a-z0-9]+$' || die "$dir/bundled.txt gives $soname the package '$owner'"
    printf '%s' "$hash" | grep -Eq '^[0-9a-f]{64}$' || die "$dir/bundled.txt gives $soname the SHA-256 '$hash'"
    printf '%s' "$source_package" | grep -Eq '^[a-z0-9][a-z0-9.+-]+$' || die "$dir/bundled.txt gives $soname the source package '$source_package'"
    printf '%s %s' "$version" "$source_version" | grep -Eq '^([0-9]+:)?[0-9][A-Za-z0-9.+~-]* ([0-9]+:)?[0-9][A-Za-z0-9.+~-]*$' \
      || die "$dir/bundled.txt gives $soname the versions '$version' and '$source_version'"
    licence_of "$source_package" > /dev/null \
      || die "$dir/bundled.txt: the image carries $soname from $owner $version, built from $source_package $source_version, a source package this script does not know: name it in licence_of, with its licence as its copyright file gives it, and say in docs/PackagesAppImage.md what that licence asks"
    printf '%s %s %s %s %s %s\n' "$source_package" "$source_version" "${owner##*:}" "$owner" "$version" "$soname"
  done 3< "$dir/bundled.txt"
}

check() {
  [ "$#" -gt 0 ] || die "usage: check DIR..."
  local dir count
  for dir in "$@"; do
    count=$(read_bundled "$dir" | wc -l)
    [ "$count" -gt 0 ] || die "$dir/bundled.txt lists no library"
    echo "$dir/bundled.txt: $count libraries, each from a source package whose licence is named here"
  done
}

# fetch URL FILE: one https GET, no redirect followed, retried with a growing wait (curl
# doubles it from one second, and keeps to a Retry-After: the snapshot service answers
# a burst of requests with 503 for a while); 0 when the file came.
fetch() {
  local code
  code=$(curl -sS --proto =https --max-redirs 0 --retry 8 --retry-all-errors --retry-max-time 600 --connect-timeout 30 \
    --max-time 900 -o "$2" -w '%{http_code}' "$1" 2> /dev/null) || code=000
  if [ "$code" = 200 ]; then
    return 0
  fi
  rm -f "$2"
  return 1
}

# held FILE SHA256 SIZE WHAT: the file has that SHA-256 and that size, or the run stops.
held() {
  local got size
  got=$(sha256sum "$1" | cut -d' ' -f1)
  size=$(stat -c %s "$1")
  [ "$got" = "$2" ] && [ "$size" = "$3" ] || die "$4 has the SHA-256 $got and $size bytes; $5 gives $2 and $3 bytes"
}

# missing: the wanted source packages and versions no index read so far has named.
# (By FILENAME, not NR == FNR, which an empty first file makes true of the second.)
missing() {
  awk 'FILENAME == ARGV[1] { seen[$2 " " $3] = 1; next } !(($1 " " $2) in seen)' "$work/found" "$work/wanted"
}

# index BASE: the Sources entries of the wanted source packages in BASE's three suites,
# appended to $work/found as "<base> <source> <version> <directory> <sha256> <size> <file>",
# each Sources index held to its signed InRelease; it stops once every one is found.
index() {
  local base=$1 suite component line hash size
  for suite in "$codename" "$codename-updates" "$codename-security"; do
    [ -n "$(missing)" ] || return 0
    rm -rf "$work/idx"
    mkdir -p "$work/idx"
    fetch "$base/dists/$suite/InRelease" "$work/idx/InRelease" || die "$base/dists/$suite/InRelease could not be fetched"
    gpgv --keyring "$KEYRING" --output "$work/idx/Release" "$work/idx/InRelease" > "$work/idx/gpgv.log" 2>&1 \
      || { cat "$work/idx/gpgv.log" >&2; die "$base/dists/$suite/InRelease is not signed by a key of $KEYRING"; }
    grep -qx "Suite: $suite" "$work/idx/Release" && grep -qx "Codename: $codename" "$work/idx/Release" \
      || die "$base/dists/$suite/InRelease is signed, and is not the InRelease of $suite ($codename)"
    for component in main universe restricted multiverse; do
      [ -n "$(missing)" ] || return 0
      line=$(awk -v p="$component/source/Sources.xz" '/^SHA256:/ { on = 1; next } /^[^ ]/ { on = 0 } on && $3 == p { print $1, $2; exit }' \
        "$work/idx/Release")
      [ -n "$line" ] || die "$base/dists/$suite/InRelease names no $component/source/Sources.xz"
      read -r hash size <<< "$line"
      fetch "$base/dists/$suite/$component/source/Sources.xz" "$work/idx/Sources.xz" \
        || die "$base/dists/$suite/$component/source/Sources.xz could not be fetched"
      held "$work/idx/Sources.xz" "$hash" "$size" "$base/dists/$suite/$component/source/Sources.xz" "its InRelease"
      xz -dc "$work/idx/Sources.xz" | awk -v wanted="$work/wanted" -v base="$base" '
        BEGIN { while ((getline line < wanted) > 0) { split(line, w, " "); want[w[1] " " w[2]] = 1 }; RS = ""; FS = "\n" }
        {
          pkg = ""; ver = ""; dir = ""; n = 0; insum = 0
          for (i = 1; i <= NF; i++) {
            if ($i ~ /^Package: /) pkg = substr($i, 10)
            else if ($i ~ /^Version: /) ver = substr($i, 10)
            else if ($i ~ /^Directory: /) dir = substr($i, 12)
            if ($i ~ /^Checksums-Sha256:/) { insum = 1; continue }
            if (insum && $i ~ /^ /) { sums[++n] = $i; continue }
            insum = 0
          }
          if ((pkg " " ver) in want)
            for (j = 1; j <= n; j++) { split(sums[j], f, " "); print base, pkg, ver, dir, f[1], f[2], f[3] }
        }' >> "$work/found"
    done
  done
}

version_h() {
  sed -n 's/^#define[[:space:]]*HMAILSERVER_VERSION[[:space:]]*"\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)".*/\1/p' \
    hmailserver/source/Server/Common/Application/Version.h | tr -d '\r'
}

pack() {
  if [ "$#" -eq 0 ]; then
    for dir in appimage/built-against-*; do
      if [ -f "$dir/bundled.txt" ]; then
        set -- "$@" "$dir"
      fi
    done
  fi
  [ "$#" -gt 0 ] || die "no records: the appimage-<arch> jobs write appimage/built-against-<arch>/bundled.txt"
  [ -f hmailserver/source/Server/Common/Application/Version.h ] || die "run this from the root of a checkout"
  version=$(version_h)
  [ -n "$version" ] || die "Version.h names no HMAILSERVER_VERSION"
  out="${HM_OUT_DIR:-appimage-bundled-sources}"
  name="hmailserver-$version-appimage-bundled-sources.tar.gz"
  top="hmailserver-$version-appimage-bundled-sources"

  if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    if [ -n "${CI_COMMIT_TIMESTAMP:-}" ]; then
      SOURCE_DATE_EPOCH=$(date -d "$CI_COMMIT_TIMESTAMP" +%s)
    elif command -v git > /dev/null 2>&1; then
      SOURCE_DATE_EPOCH=$(git -c safe.directory="$PWD" log -1 --format=%ct HEAD 2> /dev/null) || SOURCE_DATE_EPOCH=""
    fi
  fi
  printf '%s' "${SOURCE_DATE_EPOCH:-}" | grep -Eq '^[0-9]+$' \
    || die "SOURCE_DATE_EPOCH is '${SOURCE_DATE_EPOCH:-}', not a number of seconds, and neither CI_COMMIT_TIMESTAMP nor git gives one"
  echo "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"

  if ! command -v curl > /dev/null 2>&1 || ! command -v xz > /dev/null 2>&1 || [ ! -f /etc/ssl/certs/ca-certificates.crt ]; then
    DEBIAN_FRONTEND=noninteractive apt-get update -qq -o Acquire::Retries=3
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq -o Acquire::Retries=3 --no-install-recommends \
      curl xz-utils ca-certificates > /dev/null
  fi
  command -v gpgv > /dev/null 2>&1 || die "no gpgv here: run this in the ubuntu image the job names"
  [ -s "$KEYRING" ] || die "no $KEYRING here: run this in the ubuntu image the job names"

  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  stage="$work/stage/$top"
  mkdir -p "$stage"

  # The records: the distribution they share, the times to look for a version the
  # archive has dropped (when the job's apt read it and its indexes' dates, and when
  # dpkg installed each package, as "<package>:<arch> <time>"), and every library as
  # "<source> <version> <arch> <package> <package version> <soname>".
  codename=""
  arches=""
  : > "$work/uses"
  : > "$work/times"
  : > "$work/installed"
  for dir in "$@"; do
    [ -s "$dir/archive.txt" ] || die "$dir/archive.txt is missing or empty: the AppImage job writes it (appimage-build.sh)"
    distribution=$(sed -n 's/^distribution \(ubuntu [0-9][0-9.]* [a-z]*\)$/\1/p' "$dir/archive.txt")
    arch=$(sed -n 's/^arch \([a-z0-9_]*\)$/\1/p' "$dir/archive.txt")
    [ -n "$distribution" ] && [ -n "$arch" ] || die "$dir/archive.txt names no Ubuntu release or architecture"
    this_codename=${distribution##* }
    [ -z "$codename" ] || [ "$codename" = "$this_codename" ] || die "the records are of two Ubuntu releases, $codename and $this_codename"
    codename=$this_codename
    case " $arches " in
      *" $arch "*) die "two records of $arch" ;;
    esac
    arches="$arches $arch"
    sed -n 's/^\(updated\|index [a-z-]*\) \([0-9]\{8\}T[0-9]\{6\}Z\)$/\2/p' "$dir/archive.txt" >> "$work/times"
    sed -n 's/^installed \([a-z0-9][a-z0-9.+-]*:[a-z0-9]*\) \([0-9]\{8\}T[0-9]\{6\}Z\)$/\1 \2/p' "$dir/archive.txt" >> "$work/installed"
    read_bundled "$dir" >> "$work/uses"
    cp "$dir/bundled.txt" "$stage/bundled-$arch.txt"
    cp "$dir/archive.txt" "$stage/archive-$arch.txt"
  done
  arches=${arches# }
  LC_ALL=C sort -u "$work/uses" -o "$work/uses"
  cut -d' ' -f1,2 "$work/uses" | LC_ALL=C sort -u > "$work/wanted"
  # Newest first: the time apt read the archive, at which its snapshot indexes what
  # was installed, comes before the older dates of the indexes themselves.
  LC_ALL=C sort -r -u "$work/times" -o "$work/times"
  # Every package a library came from, with the time dpkg installed it.
  awk 'FILENAME == ARGV[1] { at[$1] = 1; next } !($4 in at) { print $4 }' "$work/installed" "$work/uses" | LC_ALL=C sort -u > "$work/untimed"
  [ ! -s "$work/untimed" ] || die "the records give no time dpkg installed $(tr '\n' ' ' < "$work/untimed")(an 'installed <package>:<arch> <time>' line of archive.txt, which appimage-build.sh writes)"
  echo "Ubuntu $codename; the records of: $arches; $(wc -l < "$work/wanted") source packages"

  # Where each is: the archive first, then its snapshots. A package the image's base
  # already had is at the version it had when the base was made, which apt-get install
  # does not change, and the archive may have superseded it since, before the job's
  # apt read it: only a snapshot of when dpkg installed it indexes it then. So first
  # the snapshots of the times dpkg installed each package built from a version the
  # archive does not index, newest first; then those of when the job's apt read the
  # archive and of its indexes' dates, for a version dropped after that.
  : > "$work/found"
  index "$ARCHIVE"
  missing > "$work/left"
  {
    awk 'FILENAME == ARGV[1] { gone[$1 " " $2] = 1; next }
         FILENAME == ARGV[2] { if (($1 " " $2) in gone) owner[$4] = 1; next }
         ($1 in owner) { print $2 }' "$work/left" "$work/uses" "$work/installed" | LC_ALL=C sort -r -u
    cat "$work/times"
  } | awk '!seen[$0]++' > "$work/order"
  while read -r stamp <&3; do
    left=$(missing)
    [ -n "$left" ] || break
    echo "not in the archive's indexes now: $(printf '%s\n' "$left" | tr '\n' ' ')- looking in its snapshot of $stamp"
    index "$SNAPSHOTS/$stamp"
  done 3< "$work/order"
  left=$(missing)
  [ -z "$left" ] || die "neither $ARCHIVE nor its snapshots at $(tr '\n' ' ' < "$work/order")index $(printf '%s\n' "$left" | tr '\n' ',')"

  # Each source package at each version: from the first place that indexes it, every
  # file held to the Sources index and then to the .dsc.
  : > "$work/readme-packages"
  : > "$work/readme-files"
  while read -r source_package source_version <&3; do
    awk -v p="$source_package" -v v="$source_version" '$2 == p && $3 == v' "$work/found" > "$work/entries"
    base=$(head -n 1 "$work/entries" | cut -d' ' -f1)
    awk -v b="$base" '$1 == b' "$work/entries" | cut -d' ' -f4- | LC_ALL=C sort -u > "$work/files"
    directory=$(cut -d' ' -f1 "$work/files" | LC_ALL=C sort -u)
    [ "$(printf '%s\n' "$directory" | wc -l)" -eq 1 ] || die "$base indexes $source_package $source_version in two directories"
    printf '%s' "$directory" | grep -Eq '^pool/(main|universe|restricted|multiverse)/[a-z0-9]+/[a-z0-9][a-z0-9.+-]*$' \
      || die "$base gives $source_package $source_version the directory '$directory'"
    # One SHA-256 and size for each file, in every suite and snapshot that indexes it.
    twice=$(awk '{ print $7, $5, $6 }' "$work/entries" | LC_ALL=C sort -u | cut -d' ' -f1 | uniq -d)
    [ -z "$twice" ] || die "$source_package $source_version: two indexes give $(printf '%s' "$twice" | tr '\n' ' ')two checksums"
    dir="${source_package}_${source_version#*:}"
    mkdir -p "$stage/$dir"
    dsc=""
    while read -r _ hash size file <&4; do
      printf '%s' "$file" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9._+~-]*$' || die "$base gives $source_package $source_version a file '$file'"
      fetch "$base/$directory/$file" "$stage/$dir/$file" || die "$base/$directory/$file could not be fetched"
      held "$stage/$dir/$file" "$hash" "$size" "$base/$directory/$file" "the Sources index"
      case "$file" in
        *.dsc) [ -z "$dsc" ] || die "$source_package $source_version has two .dsc files"; dsc=$file ;;
      esac
      printf '%s|%s|%s|%s|%s\n' "$dir" "$file" "$hash" "$size" "$directory" >> "$work/readme-files"
    done 4< "$work/files"
    [ "$dsc" = "${source_package}_${source_version#*:}.dsc" ] || die "$source_package $source_version has no ${source_package}_${source_version#*:}.dsc"
    # The .dsc names this package and version, and every other file with its checksum.
    awk '/^Source: / { print "source", substr($0, 9) } /^Version: / { print "version", substr($0, 10) }
         /^Checksums-Sha256:/ { on = 1; next } on && /^ / { print "file", $1, $2, $3; next } { on = 0 }' \
      "$stage/$dir/$dsc" | tr -d '\r' > "$work/dsc"
    grep -qx "source $source_package" "$work/dsc" && grep -qx "version $source_version" "$work/dsc" \
      || die "$dsc does not name $source_package $source_version"
    awk '$1 == "file" { print $4 }' "$work/dsc" | LC_ALL=C sort > "$work/dsc-files"
    awk '{ print $4 }' "$work/files" | { grep -Fvx "$dsc" || true; } | LC_ALL=C sort > "$work/index-files"
    cmp -s "$work/dsc-files" "$work/index-files" \
      || die "$dsc lists $(tr '\n' ' ' < "$work/dsc-files")and the Sources index $(tr '\n' ' ' < "$work/index-files")"
    while read -r _ hash size file <&4; do
      held "$stage/$dir/$file" "$hash" "$size" "$dir/$file" "$dsc"
    done 4< <(grep '^file ' "$work/dsc")
    echo "$dir: $source_package $source_version, $(grep -c '^file ' "$work/dsc") files and the .dsc, from $base/$directory/"
    printf '%s|%s|%s|%s|%s\n' "$dir" "$source_package" "$source_version" "$directory" "$base" >> "$work/readme-packages"
  done 3< "$work/wanted"
  [ "$(wc -l < "$work/readme-packages")" -eq "$(wc -l < "$work/wanted")" ] \
    || die "$(wc -l < "$work/wanted") source packages to pack, and $(wc -l < "$work/readme-packages") packed"
  LC_ALL=C sort -t '|' -k1,1 -k2,2 -u "$work/readme-files" -o "$work/readme-files"

  write_readme > "$stage/README.txt"
  ( cd "$stage" && find . -type f | sed 's|^\./||' | LC_ALL=C sort | while IFS= read -r file; do
      sha256sum "$file"
    done ) > "$work/SHA256SUMS"
  mv "$work/SHA256SUMS" "$stage/SHA256SUMS"

  mkdir -p "$out"
  rm -f "${out:?}/$name" "${out:?}/SHA256SUMS.txt"
  # a+rX,u+w,go-w: 0644 and 0755 whatever umask the files were made under.
  LC_ALL=C tar --sort=name --format=gnu --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner \
    --mode='a+rX,u+w,go-w' -C "$work/stage" -cf - "$top" | gzip -n -9 > "$out/$name"
  ( cd "$out" && sha256sum "$name" > SHA256SUMS.txt )
  echo ""
  echo "$out/$name: $(wc -c < "$out/$name" | tr -d ' ') bytes, SHA-256 $(cut -d' ' -f1 "$out/SHA256SUMS.txt")"
  tar -t -z -f "$out/$name"
}

write_readme() {
  cat <<EOF
hMailServer $version: the source of the libraries in the AppImages' usr/lib
============================================================================

Each hMailServer AppImage of this release (hMailServer-$version-x86_64.AppImage and
hMailServer-$version-aarch64.AppImage) carries in usr/lib the shared libraries the
server needs that are not left to the host: build-appimage.sh copies each from the
Ubuntu $codename package that owns the file ldd resolves, in the container the image is
packed in. This archive holds the source of every one of them, exactly as Ubuntu built
it: each source package - its .dsc, the upstream archive(s) and Ubuntu's changes
(.debian.tar.* or .diff.gz) - at the version the image's package was built from.

Several of these libraries are under the LGPL, which asks for the library's source to
be offered with the binary, from the same place (LGPL-2.1 sections 4 and 6; the
LGPL-3.0 through section 6 of the GPL-3.0); this release offers it beside the
AppImages. The others ask for their notice alone, and are here too (below).

Each AppImage lists what it carries in usr/share/hmailserver/licenses/bundled.txt, with
each package's copyright file, as Ubuntu ships it, in
usr/share/hmailserver/licenses/bundled/<package>/copyright, and every licence text those
files name in /usr/share/common-licenses in usr/share/hmailserver/licenses/bundled/common-licenses/.
bundled-<arch>.txt here are the lists the images' builds recorded, with the SHA-256 of
each package's file, and archive-<arch>.txt when their apt read Ubuntu's archive.
SHA256SUMS lists every file here.

The source packages
-------------------

EOF
  while IFS='|' read -r dir source_package source_version directory base; do
    decision=$(licence_of "$source_package")
    printf '%s/\n' "$dir"
    # Where it was taken from: the archive, or the snapshot that still indexed it.
    printf '  %s %s, from %s/%s/\n' "$source_package" "$source_version" "$base" "$directory"
    printf '  Licence of the libraries taken from it: %s\n' "${decision#*|}"
    case "${decision%%|*}" in
      source) printf '  It asks for: the source, offered with the binary from the same place; the copyright file and the licence text\n' ;;
      *) printf '  It asks for: the notice, in the copyright file; the source is here all the same\n' ;;
    esac
    printf '  In the AppImages:\n'
    awk -v p="$source_package" -v v="$source_version" '$1 == p && $2 == v { printf "    %s usr/lib/%s, from %s %s\n", $3, $6, $4, $5 }' "$work/uses"
    printf '  Files, each with the SHA-256 the Sources index gives it (and the .dsc, for every file but itself):\n'
    while IFS='|' read -r file_dir file hash size _; do
      [ "$file_dir" = "$dir" ] || continue
      printf '    %s  %s (%s bytes)\n' "$hash" "$file" "$size"
    done < "$work/readme-files"
    printf '\n'
  done < "$work/readme-packages"
  cat <<EOF
Where each file came from, and how it was checked
-------------------------------------------------

Every file above was fetched from Ubuntu's archive, $ARCHIVE/<directory>/<file>, or,
for a version the archive no longer indexed when this was made, from Ubuntu's snapshot
service at a time the image's build recorded, $SNAPSHOTS/<time>/<directory>/<file>
(the times are in archive-<arch>.txt). Nothing came from anywhere else. Each is held to
the SHA-256 and size of one chain:

  1. the InRelease of $codename, $codename-updates and $codename-security, verified with
     gpgv against Ubuntu's archive signing keys (ubuntu-keyring's
     /usr/share/keyrings/ubuntu-archive-keyring.gpg);
  2. each suite's Sources index, held to the SHA-256 and size its InRelease gives;
  3. the .dsc and every other file of the source package, held to the SHA-256 and size
     the Sources index gives;
  4. every file the .dsc lists, held again to the SHA-256 and size the .dsc gives, the
     .dsc naming the source package and version the image's record names.

To check one again: apt-get source --download-only <source>=<version> on Ubuntu
$codename with deb-src entries, or dscverify, or compare the SHA-256 above.

Building a library again, as Ubuntu did
---------------------------------------

On Ubuntu $codename, in a directory of this archive:

  dpkg-source -x <source>_<version>.dsc
  cd <source>-<upstream version>
  apt-get build-dep .
  dpkg-buildpackage -b -uc -us

Running an AppImage with a changed library
------------------------------------------

The server finds these libraries through its RUNPATH, \$ORIGIN/../lib, and each of them
the others through its own, \$ORIGIN: whatever file is at usr/lib/<soname> in the
image is the one it loads (section 6 of the LGPL-2.1, a suitable shared library
mechanism; 4(d)(1) of the LGPL-3.0). To use a changed copy:

  ./hMailServer-$version-<arch>.AppImage --appimage-extract
  cp libchanged.so squashfs-root/usr/lib/<soname>
  patchelf --set-rpath '\$ORIGIN' squashfs-root/usr/lib/<soname>
  squashfs-root/AppRun --version

and to make an AppImage of it again, the release's runtime (the first
--appimage-offset bytes of the AppImage) followed by the new image:

  offset=\$(./hMailServer-$version-<arch>.AppImage --appimage-offset)
  head -c "\$offset" hMailServer-$version-<arch>.AppImage > runtime
  mksquashfs squashfs-root image.squashfs -root-owned -noappend -comp zstd
  cat runtime image.squashfs > hMailServer-changed.AppImage
  chmod +x hMailServer-changed.AppImage

hMailServer itself is under the AGPL-3.0-or-later: its source is this release's
hmailserver-$version.tar.gz, and its text is usr/share/hmailserver/licenses/AGPL-3.0.txt
in each AppImage. The source of the libraries linked into the AppImages' runtime is
hmailserver-$version-appimage-runtime-sources.tar.gz.
EOF
}

case "${1:-}" in
  check) shift; check "$@" ;;
  pack) shift; pack "$@" ;;
  *) echo "usage: $0 check DIR... | pack [DIR...]" >&2; exit 2 ;;
esac
