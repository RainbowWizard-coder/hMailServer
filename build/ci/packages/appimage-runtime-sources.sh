#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The source of the libraries linked into the AppImage runtime, as a release file of
# its own: hmailserver-<version>-appimage-runtime-sources.tar.gz
# (docs/PackagesAppImage.md, "Licences").
#
# Why: the runtime at the front of every AppImage (build/ci/packages/appimage-runtime.sh)
# is linked statically against Alpine's packages, and two of the libraries it takes in
# ask for their own source to go with the binary: libfuse3, under the LGPL-2.1
# (section 6), and lzo, under the GPL-2.0-or-later (section 3; the runtime takes it
# under the GPL-3.0 the "or later" allows, since runtime.c is AGPL-3.0-or-later, and
# the GPL-3.0's section 6 asks the same). Because lzo's licence is the GPL, the runtime
# is a work based on it, and its corresponding source is the source of everything
# linked into it that is not a system library: so the libraries whose own licences ask
# only for their notice go in too - squashfuse, zstd, lz4, xz's liblzma, zlib - and
# musl, the C library, which the runtime carries onto systems whose C library is
# another. Not here: gcc's start-up files and libgcc, which the compiler links into
# every program it makes. They are under the GPL-3.0-or-later with the GCC Runtime
# Library Exception 3.1, which lets a program compiled by GCC be conveyed under terms
# of its own choosing, and are the compiler's System Libraries in the GPL-3.0's words;
# README.txt names the files and where Alpine's source of them is.
#
# Nothing here is fetched from GitHub, although several of these libraries publish
# their releases there: each upstream archive comes from Alpine's own distfiles mirror
# (https://distfiles.alpinelinux.org/distfiles/<branch>/), and Alpine's build script
# and patches from aports on Alpine's GitLab (https://gitlab.alpinelinux.org), at the
# commit apk's database records for the installed package. No redirect is followed.
#
#   sh build/ci/packages/appimage-runtime-sources.sh record MAP OUT
#
#     In the appimage-runtime-<arch> job, after the link: every file the linker's map
#     MAP says went into the runtime - each archive a member was taken from, each object
#     file loaded - with the Alpine package that owns it and that package's version,
#     origin (its aports directory), the aports commit it was built from and its
#     licence, all read from apk's own database, written to OUT
#     (appimage/runtime-<arch>-linked.txt). A linked file no package owns, other than
#     the compiler's own objects of runtime.c, or a package whose origin is not named
#     in PACKED or NOT_PACKED below, fails the job: a library the runtime starts to
#     link is looked at, and its licence decided, before an AppImage carries it.
#
#   sh build/ci/packages/appimage-runtime-sources.sh pack [RECORD...]
#
#     In the appimage-runtime-sources job (alpine, the runtime jobs' image), from the
#     records of both runtime jobs (appimage/runtime-*-linked.txt by default): for each
#     library of PACKED at each version and commit the records name, Alpine's aports
#     directory at that commit (git, fetched by the commit's id, so every file is the
#     commit's), the APKBUILD read for each architecture that linked it - its version
#     must be the installed one - and every source it lists, each file held to the
#     SHA-512 the APKBUILD gives it: the aports files as they are, the upstream archives
#     fetched from the distfiles mirror of the records' Alpine branch (edge's when the
#     branch's has none). Then each library's licence files, from its upstream archive,
#     into licenses-<arch>/<library>/ for that architecture's AppImage
#     (build-appimage.sh --runtime-licenses), and the whole, with README.txt, the
#     records and SHA512SUMS, packed reproducibly: sorted, every entry dated
#     SOURCE_DATE_EPOCH (the commit's time when it is not set), owned by 0:0, modes 0644
#     and 0755, GNU tar's own format, gzip -n -9. HM_OUT_DIR (appimage-runtime-sources)
#     gets the tarball, SHA256SUMS.txt naming it, and the licences.
#
# Run in alpine:3.24 from the root of a checkout; pack needs network access to the two
# Alpine hosts above and installs git, curl, GNU tar, gzip and xz with apk. Every step
# that reaches the network is tried again for some minutes before the job fails: an
# outage of Alpine's mirror or GitLab that short no longer stops a tag's pipeline, and
# what arrives is held to the same SHA-512s and commit ids as before.
set -eu

# The libraries whose source the release file carries, by aports origin, and the files
# of each upstream archive (below its top directory) that hold the licence the runtime
# takes it under. zstd is BSD-3-Clause OR GPL-2.0: the runtime takes the BSD licence
# (LICENSE); COPYING, the GPL's text, goes with it. lz4's library is BSD-2-Clause
# (lib/LICENSE); its programs, which the runtime does not link, are GPL-2.0. fuse3's
# LICENSE says which of its files are under the LGPL-2.1 (the library's) and which
# under the GPL-2.0 (its programs, not linked); LGPL2.txt is the LGPL's text.
PACKED=" fuse3 lz4 lzo musl squashfuse xz zlib zstd "
licence_files() {
  case "$1" in
    fuse3) echo "LICENSE LGPL2.txt" ;;
    lz4) echo "lib/LICENSE" ;;
    lzo) echo "COPYING" ;;
    musl) echo "COPYRIGHT" ;;
    squashfuse) echo "LICENSE" ;;
    xz) echo "COPYING COPYING.0BSD" ;;
    zlib) echo "LICENSE" ;;
    zstd) echo "LICENSE COPYING" ;;
    *) return 1 ;;
  esac
}
# Linked into the runtime and not carried; README.txt says why.
NOT_PACKED=" gcc "

APORTS="https://gitlab.alpinelinux.org/alpine/aports.git"
DISTFILES="https://distfiles.alpinelinux.org/distfiles"

die() {
  echo "appimage-runtime-sources: $*" >&2
  exit 1
}

# listed LIST WORD: whether WORD is one of the space-separated words of LIST.
listed() {
  case " $1 " in
    *" $2 "*) return 0 ;;
  esac
  return 1
}

# ---------------------------------------------------------------- record

record() {
  [ "$#" -eq 2 ] || die "usage: record MAP OUT"
  map=$1
  out=$2
  [ -s "$map" ] || die "no linker map at $map"
  [ -f /etc/alpine-release ] || die "this reads apk's database: run it in the alpine image the job names"
  [ -s /lib/apk/db/installed ] || die "apk's database, /lib/apk/db/installed, is not here"
  scratch=$(mktemp -d)
  # Every installed package: name, version, origin, the aports commit, the licence.
  awk '
    function flush() { if (p != "") print p, v, o, (c == "" ? "-" : c), (l == "" ? "-" : l); p = v = o = c = l = "" }
    /^$/ { flush(); next }
    /^P:/ { p = substr($0, 3) } /^V:/ { v = substr($0, 3) } /^o:/ { o = substr($0, 3) }
    /^c:/ { c = substr($0, 3) } /^L:/ { l = substr($0, 3) }
    END { flush() }' /lib/apk/db/installed > "$scratch/db"
  # What the link took: each archive a member was included from, then each object file
  # it loaded. An archive the link opened and took nothing from is not linked.
  awk '
    /^Archive member included/ { on = 1; next }
    on && /^(Discarded input sections|Allocating common symbols|Memory Configuration|Linker script and memory map)/ { on = 0 }
    on && /^\// { sub(/\(.*/, ""); print }
    /^LOAD \// && $2 ~ /\.o$/ { print $2 }' "$map" > "$scratch/paths"
  : > "$scratch/linked"
  # The list on descriptor 3, not standard input, which a command in the loop could
  # read from and so take paths out of the record unseen.
  while IFS= read -r path <&3; do
    real=$(realpath "$path")
    owner=$(apk info --who-owns "$real" 2> /dev/null | sed -n 's/^.* is owned by //p' | head -n 1)
    if [ -z "$owner" ]; then
      case "$(basename "$real")" in
        # The compiler's own objects of runtime.c, and link-time optimisation's.
        cc*.o) continue ;;
      esac
      die "the runtime links $real, which no Alpine package owns"
    fi
    fields=$(awk -v owned="$owner" '$1 "-" $2 == owned { print; exit }' "$scratch/db")
    [ -n "$fields" ] || die "apk says $owner owns $real, and its database has no $owner"
    package=$(printf '%s\n' "$fields" | cut -d' ' -f1)
    version=$(printf '%s\n' "$fields" | cut -d' ' -f2)
    origin=$(printf '%s\n' "$fields" | cut -d' ' -f3)
    commit=$(printf '%s\n' "$fields" | cut -d' ' -f4)
    licence=$(printf '%s\n' "$fields" | cut -d' ' -f5-)
    printf '%s' "$commit" | grep -Eq '^[0-9a-f]{40}$' \
      || die "$package $version records no aports commit (c:), so the source it was built from cannot be named"
    if ! listed "$PACKED" "$origin" && ! listed "$NOT_PACKED" "$origin"; then
      die "the runtime links $real from $package $version (origin $origin, $licence), a library this script does not know: name it in PACKED, with its licence files, or in NOT_PACKED with the reason in README.txt"
    fi
    printf 'linked %s %s %s %s %s %s\n' "$real" "$package" "$version" "$origin" "$commit" "$licence" >> "$scratch/linked"
  done 3< "$scratch/paths"
  if ! grep -q ' fuse3 ' "$scratch/linked" || ! grep -q ' squashfuse ' "$scratch/linked"; then
    die "the map names no member of libfuse3 or squashfuse: it is not this runtime's link"
  fi
  {
    echo "# What build/ci/packages/appimage-runtime.sh linked into runtime-$(uname -m), from the linker's map,"
    echo "# each file with the Alpine package that owns it (apk's database): the package, its version,"
    echo "# its origin, the aports commit it was built from, its licence (build/ci/packages/appimage-runtime-sources.sh)."
    echo "alpine $(cat /etc/alpine-release)"
    echo "arch $(uname -m)"
    LC_ALL=C sort -u "$scratch/linked"
  } > "$out"
  rm -rf "$scratch"
  cat "$out"
}

# ---------------------------------------------------------------- pack

version_h() {
  sed -n 's/^#define[[:space:]]*HMAILSERVER_VERSION[[:space:]]*"\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)".*/\1/p' \
    hmailserver/source/Server/Common/Application/Version.h | tr -d '\r'
}

# fetch URL FILE: one https GET, no redirect followed; 0 when the file came. A failed
# connection, a timeout, a reset or a 408, 429 or 5xx answer is tried again with a
# growing wait (curl doubles it from one second, and keeps to a Retry-After) for up to
# ten minutes; an answer such as 404, which says the file is not there, is not, so
# that the branch's mirror missing a file still falls through to edge's at once.
fetch() {
  code=$(curl -sS --proto =https --max-redirs 0 --retry 8 --retry-all-errors --retry-max-time 600 \
    --connect-timeout 30 --max-time 900 -o "$2" -w '%{http_code}' "$1" 2> /dev/null) || code=000
  if [ "$code" = 200 ]; then
    return 0
  fi
  rm -f "$2"
  return 1
}

sha512_of() {
  sha512sum "$1" | cut -d' ' -f1
}

# attempt WHAT COMMAND...: the command, and twice more after 30 and then 60 seconds
# when it fails; its status the last time.
attempt() {
  what=$1
  shift
  for wait in 30 60 none; do
    if "$@"; then
      return 0
    fi
    [ "$wait" = none ] && return 1
    echo "$what failed; trying again in $wait seconds"
    sleep "$wait"
  done
}

pack() {
  if [ "$#" -eq 0 ]; then
    for record_file in appimage/runtime-*-linked.txt; do
      if [ -f "$record_file" ]; then
        set -- "$@" "$record_file"
      fi
    done
  fi
  [ "$#" -gt 0 ] || die "no records: the appimage-runtime-* jobs write appimage/runtime-<arch>-linked.txt"
  [ -f hmailserver/source/Server/Common/Application/Version.h ] || die "run this from the root of a checkout"
  [ -f /etc/alpine-release ] || die "run this in the alpine image the job names"
  version=$(version_h)
  [ -n "$version" ] || die "Version.h names no HMAILSERVER_VERSION"
  out="${HM_OUT_DIR:-appimage-runtime-sources}"
  name="hmailserver-$version-appimage-runtime-sources.tar.gz"
  top="hmailserver-$version-appimage-runtime-sources"

  attempt "apk add" apk add --no-cache --quiet git curl tar gzip xz || die "apk could not install git, curl, tar, gzip and xz"

  if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    SOURCE_DATE_EPOCH=$(git -c safe.directory="$PWD" log -1 --format=%ct HEAD 2> /dev/null) \
      || die "SOURCE_DATE_EPOCH is not set and git cannot read the commit's time here"
  fi
  printf '%s' "$SOURCE_DATE_EPOCH" | grep -Eq '^[0-9]+$' || die "SOURCE_DATE_EPOCH is '$SOURCE_DATE_EPOCH', not a number of seconds"
  echo "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"

  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  stage="$work/stage/$top"
  mkdir -p "$stage" "$work/licenses" "$work/dist" "$work/out-licenses"

  # The records: the Alpine branch they share, and every linked library to pack as
  # "origin version commit arch".
  branch=""
  arches=""
  : > "$work/units"
  : > "$work/not-packed"
  for record_file in "$@"; do
    [ -s "$record_file" ] || die "$record_file is empty"
    release=$(sed -n 's/^alpine \([0-9][0-9.]*\)$/\1/p' "$record_file")
    arch=$(sed -n 's/^arch \([a-z0-9_]*\)$/\1/p' "$record_file")
    [ -n "$release" ] && [ -n "$arch" ] || die "$record_file names no Alpine release or architecture"
    this_branch="v$(echo "$release" | cut -d. -f1-2)"
    [ -z "$branch" ] || [ "$branch" = "$this_branch" ] || die "the records are of two Alpine branches, $branch and $this_branch"
    branch=$this_branch
    if listed "$arches" "$arch"; then
      die "two records of $arch"
    fi
    arches="$arches $arch"
    cp "$record_file" "$stage/linked-$arch.txt"
    while read -r kind path package pkgversion origin commit licence; do
      [ "$kind" = linked ] || continue
      printf '%s' "$commit" | grep -Eq '^[0-9a-f]{40}$' || die "$record_file gives $package the aports commit '$commit'"
      printf '%s %s' "$origin" "$pkgversion" | grep -Eq '^[a-z0-9][a-z0-9._+-]* [0-9][A-Za-z0-9._+~-]*-r[0-9]+$' \
        || die "$record_file gives $package the origin '$origin' and version '$pkgversion'"
      if listed "$PACKED" "$origin"; then
        echo "$origin $pkgversion $commit $arch" >> "$work/units"
      elif listed "$NOT_PACKED" "$origin"; then
        echo "$arch $path $package $pkgversion $origin $commit $licence" >> "$work/not-packed"
      else
        die "$record_file names $path from $package $pkgversion (origin $origin), a library this script does not know"
      fi
    done < "$record_file"
  done
  LC_ALL=C sort -u "$work/units" -o "$work/units"
  [ -s "$work/units" ] || die "the records name no library to pack"
  echo "Alpine $branch; the records of:$arches"

  # aports at every commit named, by id: git checks every object it is given.
  aports="$work/aports"
  git init -q "$aports"
  git -C "$aports" config http.followRedirects false
  git -C "$aports" config protocol.version 2
  git -C "$aports" remote add origin "$APORTS"
  cut -d' ' -f3 "$work/units" | LC_ALL=C sort -u > "$work/commits"
  fetch_aports() {
    xargs git -C "$aports" fetch -q --no-tags --depth 1 --filter=blob:none origin < "$work/commits"
  }
  attempt "fetching aports" fetch_aports \
    || die "aports could not be fetched at $(tr '\n' ' ' < "$work/commits")from $APORTS"

  # Each library, version and commit: where it is in aports, and that directory's tree. The
  # fetch brought the trees (--filter=blob:none), so the lookup reads nothing from the
  # network: a git failure here is not a missing directory, and is tried again.
  aports_where() {
    for wait in 30 60 none; do
      if listed=$(git -C "$aports" ls-tree -d "$1" "main/$2" "community/$2" 2>&1); then
        printf '%s\n' "$listed" | awk '$2 == "tree" { print $4, $3; exit }'
        return 0
      fi
      if [ "$wait" = none ]; then
        printf '%s\n' "$listed" >&2
        return 1
      fi
      echo "aports' directory of $2 at $1 could not be read; trying again in $wait seconds" >&2
      sleep "$wait"
    done
  }
  cut -d' ' -f1-3 "$work/units" | LC_ALL=C sort -u > "$work/ovc"
  : > "$work/trees"
  while read -r origin pkgversion commit; do
    found=$(aports_where "$commit" "$origin") || die "aports could not be read at $commit"
    [ -n "$found" ] || die "aports has no main/$origin or community/$origin at $commit"
    where=${found% *}
    tree=${found#* }
    echo "$origin $pkgversion $commit $where $tree" >> "$work/trees"
  done < "$work/ovc"
  # The list the loop below reads, and a copy it looks things up in.
  cp "$work/trees" "$work/trees.all"

  : > "$work/readme-libraries"
  : > "$work/readme-files"
  # The list on descriptor 3, and each APKBUILD sourced with nothing on standard
  # input: nothing run in the loop can take a library off the list unseen.
  while read -r origin pkgversion commit where tree <&3; do
    # One directory per library and version; one per commit where the architectures'
    # packages of one version were built from aports directories that differ.
    trees=$(awk -v o="$origin" -v v="$pkgversion" '$1 == o && $2 == v { print $5 }' "$work/trees.all" | sort -u | wc -l)
    dir="$origin-$pkgversion"
    [ "$trees" -eq 1 ] || dir="$origin-$pkgversion-$(echo "$commit" | cut -c1-12)"
    target="$stage/$dir"
    uses=$(awk -v o="$origin" -v v="$pkgversion" -v c="$commit" '$1 == o && $2 == v && $3 == c { print $4 }' "$work/units" \
      | tr '\n' ' ' | sed 's/ *$//')
    mkdir -p "$target"
    rm -rf "$work/x"
    mkdir -p "$work/x"
    # The archive fetches the directory's blobs, which the fetch left out: a lapse is tried again.
    attempt "aports' $where at $commit" git -C "$aports" archive --format=tar -o "$work/aports.tar" "$commit" "$where" \
      || die "aports' $where could not be read at $commit"
    tar -x -f "$work/aports.tar" -C "$work/x"
    if [ -n "$(find "$work/x" -type l)" ]; then
      die "aports' $where at $commit holds a symbolic link"
    fi
    cp -R "$work/x/$where/." "$target/"
    [ -f "$target/APKBUILD" ] || die "aports' $where at $commit has no APKBUILD"

    # The APKBUILD as abuild reads it, for each architecture that linked this one.
    : > "$work/sums"
    : > "$work/sources"
    licence=""
    for arch in $uses; do
      rm -f "$work/apkbuild.out"
      (
        set +eu
        cd "$target" || exit 1
        CARCH=$arch
        CBUILD="$arch-alpine-linux-musl"
        CHOST=$CBUILD
        CTARGET=$CBUILD
        srcdir="$work/srcdir"
        pkgdir="$work/pkgdir"
        startdir=$target
        export CARCH CBUILD CHOST CTARGET srcdir pkgdir startdir
        # shellcheck disable=SC1091
        . ./APKBUILD > /dev/null 2>&1
        # What abuild reads from an APKBUILD, which sets these.
        # shellcheck disable=SC2154
        printf 'pkgver=%s\npkgrel=%s\nlicense=%s\n' "$pkgver" "$pkgrel" "$license" > "$work/apkbuild.out"
        # shellcheck disable=SC2154
        printf '%s\n' "$sha512sums" >> "$work/sums"
        # shellcheck disable=SC2154
        for entry in $source; do
          printf '%s\n' "$entry"
        done >> "$work/sources"
      ) < /dev/null || die "$dir's APKBUILD could not be read for $arch"
      [ -s "$work/apkbuild.out" ] || die "$dir's APKBUILD could not be read for $arch"
      said="$(sed -n 's/^pkgver=//p' "$work/apkbuild.out")-r$(sed -n 's/^pkgrel=//p' "$work/apkbuild.out")"
      [ "$said" = "$pkgversion" ] \
        || die "aports' $where at $commit builds $said, and the package installed for $arch is $pkgversion"
      licence=$(sed -n 's/^license=//p' "$work/apkbuild.out")
    done
    # Every source's file name, with the SHA-512 the APKBUILD gives it, once.
    grep -E '^[0-9a-f]{128}  [^ ]+$' "$work/sums" | LC_ALL=C sort -u > "$work/sums.sorted" || true
    [ -s "$work/sums.sorted" ] || die "$dir's APKBUILD lists no sha512sums"
    if [ -n "$(cut -d' ' -f3 "$work/sums.sorted" | sort | uniq -d)" ]; then
      die "$dir's APKBUILD gives one file two SHA-512s for two architectures"
    fi
    while IFS= read -r entry; do
      [ -n "$entry" ] || continue
      case "$entry" in
        *::*) file=${entry%%::*} ;;
        *://*) file=${entry##*/} ;;
        *) file=$entry ;;
      esac
      cut -d' ' -f3 "$work/sums.sorted" | grep -Fqx "$file" || die "$dir's APKBUILD lists the source $file and no SHA-512 for it"
    done < "$work/sources"

    : > "$work/archives"
    while read -r hash file; do
      printf '%s' "$file" | grep -Eq '^[A-Za-z0-9_][A-Za-z0-9._+-]*$' || die "$dir's APKBUILD names a source '$file'"
      if [ -f "$work/x/$where/$file" ]; then
        from="aports"
      else
        # An upstream archive: from the mirror, once for every directory that lists it.
        if [ ! -f "$work/dist/$file" ]; then
          for place in "$branch" edge; do
            if fetch "$DISTFILES/$place/$file" "$work/dist/$file"; then
              echo "$DISTFILES/$place/$file" > "$work/dist/$file.from"
              break
            fi
          done
          [ -f "$work/dist/$file" ] || die "$file ($dir) is on neither $DISTFILES/$branch/ nor $DISTFILES/edge/"
        fi
        cp "$work/dist/$file" "$target/$file"
        from=$(cat "$work/dist/$file.from")
        echo "$file" >> "$work/archives"
      fi
      got=$(sha512_of "$target/$file")
      [ "$got" = "$hash" ] || die "$dir/$file ($from) has the SHA-512 $got; the APKBUILD gives $hash"
      echo "  $dir/$file: the SHA-512 the APKBUILD gives ($from)"
      printf '%s|%s|%s\n' "$dir" "$file" "$from" >> "$work/readme-files"
    done < "$work/sums.sorted"

    # The licence files, from the upstream archive, for each architecture's AppImage.
    wanted=$(licence_files "$origin") || die "no licence files are named for $origin"
    rm -rf "$work/licenses/$dir"
    mkdir -p "$work/licenses/$dir"
    while IFS= read -r file; do
      case "$file" in
        *.tar.gz|*.tgz|*.tar.xz|*.tar.bz2)
          ;;
        *)
          continue
          ;;
      esac
      upstream_top=$(tar -t -f "$target/$file" | head -n 1 | cut -d/ -f1)
      for wanted_file in $wanted; do
        rm -rf "$work/lic"
        mkdir -p "$work/lic"
        if tar -x -f "$target/$file" -C "$work/lic" "$upstream_top/$wanted_file" 2> /dev/null; then
          cp "$work/lic/$upstream_top/$wanted_file" "$work/licenses/$dir/$(basename "$wanted_file")"
        fi
      done
    done < "$work/archives"
    for wanted_file in $wanted; do
      [ -f "$work/licenses/$dir/$(basename "$wanted_file")" ] \
        || die "$dir's upstream archive has no $wanted_file, $origin's licence: name its licence files in licence_files"
    done
    for arch in $uses; do
      mkdir -p "$work/out-licenses/licenses-$arch/$origin"
      cp "$work/licenses/$dir/"* "$work/out-licenses/licenses-$arch/$origin/"
    done
    echo "$dir|$origin|$pkgversion|$commit|$where|$tree|$licence|$uses" >> "$work/readme-libraries"
    echo "$dir: $where at $commit (tree $tree), for $uses"
  done 3< "$work/trees"
  [ "$(wc -l < "$work/readme-libraries")" -eq "$(wc -l < "$work/trees")" ] \
    || die "$(wc -l < "$work/trees") libraries to pack, and $(wc -l < "$work/readme-libraries") packed"
  LC_ALL=C sort -u "$work/readme-files" -o "$work/readme-files"

  write_readme > "$stage/README.txt"
  ( cd "$stage" && find . -type f | sed 's|^\./||' | LC_ALL=C sort | while IFS= read -r file; do
      sha512sum "$file"
    done ) > "$work/SHA512SUMS"
  mv "$work/SHA512SUMS" "$stage/SHA512SUMS"

  mkdir -p "$out"
  rm -rf "${out:?}/$name" "${out:?}/SHA256SUMS.txt" "${out:?}/licenses-"*
  # a+rX,u+w,go-w: 0644 and 0755 whatever umask the files were made under.
  LC_ALL=C tar --sort=name --format=gnu --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner \
    --mode='a+rX,u+w,go-w' -C "$work/stage" -cf - "$top" | gzip -n -9 > "$out/$name"
  ( cd "$out" && sha256sum "$name" > SHA256SUMS.txt )
  cp -R "$work/out-licenses/." "$out/"
  echo ""
  echo "$out/$name: $(wc -c < "$out/$name" | tr -d ' ') bytes, SHA-256 $(cut -d' ' -f1 "$out/SHA256SUMS.txt")"
  tar -t -z -f "$out/$name"
  for arch in $arches; do
    echo "$out/licenses-$arch/:"
    ( cd "$out/licenses-$arch" && find . -type f | sed 's|^\./|  |' | LC_ALL=C sort )
  done
}

write_readme() {
  cat <<EOF
hMailServer $version: the source of the AppImage runtime's libraries
==================================================================

Each hMailServer AppImage of this release (hMailServer-$version-x86_64.AppImage and
hMailServer-$version-aarch64.AppImage) begins with the project's own AppImage runtime:
a static program built from hmailserver/source/Server/platform/packaging/appimage/runtime.c
by build/ci/packages/appimage-runtime.sh on Alpine Linux ($branch), and linked
statically against Alpine's own packages. Both files are in this release's source
tarball, hmailserver-$version.tar.gz, and in the repository.

This archive holds the exact source Alpine built each library linked into the runtime
from, as the licences of two of them ask: libfuse3 is under the LGPL-2.1 (section 6),
and lzo under the GPL-2.0-or-later (section 3), which the runtime takes under the
GPL-3.0 its "or later" allows (section 6 there). Because of lzo, the runtime is a work
based on a GPL library, and its corresponding source is that of everything linked into
it but the compiler's System Libraries - so the libraries whose own licences ask only
for their notice are here too.

What each runtime links, file by file, with the Alpine package, version, origin,
aports commit and licence apk's database gives for it, is linked-<arch>.txt, written by
the runtime's own job from the linker's map. SHA512SUMS lists every other file here.

The libraries
-------------

EOF
  while IFS='|' read -r dir origin pkgversion commit where tree licence uses; do
    case "$uses" in
      *" "*) runtimes="the $(echo "$uses" | sed 's/ / and /g') runtimes" ;;
      *) runtimes="the $uses runtime" ;;
    esac
    printf '%s/\n' "$dir"
    printf '  %s %s, linked into %s; its APKBUILD says: license="%s"\n' "$origin" "$pkgversion" "$runtimes" "$licence"
    printf '  Alpine build script and patches: %s\n    %s at commit %s (tree %s)\n' "$APORTS" "$where" "$commit" "$tree"
    while IFS='|' read -r file_dir file from; do
      [ "$file_dir" = "$dir" ] || continue
      if [ "$from" = aports ]; then
        printf '  %s: aports\n' "$file"
      else
        printf '  %s: %s\n' "$file" "$from"
      fi
    done < "$work/readme-files"
    printf '\n'
  done < "$work/readme-libraries"
  cat <<EOF
Every file above that the APKBUILD lists as a source has the SHA-512 the APKBUILD
gives it (its sha512sums), checked when this archive was made. The APKBUILD and the
other files of each aports directory are that commit's own, as git checked them by
their object ids. Each upstream archive was fetched from Alpine's distfiles mirror,
at the address given, and from nowhere else.

What the licences ask, and where it is
--------------------------------------

  libfuse3 (fuse3)   LGPL-2.1-only for the library: its source (here) and the means to
                     link the runtime again with a changed copy of it: runtime.c and
                     appimage-runtime.sh, in hmailserver-$version.tar.gz; see below.
  lzo                GPL-2.0-or-later, taken under the GPL-3.0: the complete source of
                     the runtime, which is runtime.c and appimage-runtime.sh with every
                     library here, and the GPL-3.0's text.
  squashfuse         BSD-2-Clause: its notice.
  zstd               BSD-3-Clause OR GPL-2.0, taken under BSD-3-Clause: its notice.
  lz4                the library is BSD-2-Clause: its notice.
  xz (liblzma)       0BSD: nothing.
  zlib               Zlib: nothing for a binary.
  musl               MIT: its notice.

Each AppImage carries these libraries' licence files in
usr/share/hmailserver/licenses/appimage-runtime/<library>/, beside
usr/share/hmailserver/licenses/appimage-runtime.txt, which names this file, and
the GPL-3.0's text, usr/share/hmailserver/licenses/appimage-runtime/GPL-3.0.txt
(hmailserver/source/Server/platform/packaging/appimage/GPL-3.0.txt in the source
tarball).

Linked and not here: gcc
------------------------

EOF
  if [ -s "$work/not-packed" ]; then
    LC_ALL=C sort -u "$work/not-packed" | while read -r arch path package pkgversion origin commit licence; do
      printf '  %s: %s, from %s %s (origin %s, aports commit %s)\n' "$arch" "$path" "$package" "$pkgversion" "$origin" "$commit"
    done
  else
    printf '  (nothing)\n'
  fi
  cat <<EOF

GCC's start-up files (crtbeginS.o, crtendS.o) and its runtime library (libgcc.a)
are linked by the compiler into every program it makes. They are under the
GPL-3.0-or-later with the GCC Runtime Library Exception, version 3.1, which lets a
program compiled with GCC be conveyed under terms of its own choosing, and they are
the compiler's System Libraries in the GPL-3.0's words (section 1), which a work's
corresponding source leaves out. Alpine's source of them is gcc's own archive on the
same distfiles mirror, with the build script and patches of aports' main/gcc at the
commit named above.

Building a library again, as Alpine did
---------------------------------------

On Alpine Linux ($branch), with abuild set up, in a directory of this archive:

  SRCDEST="\$PWD" abuild -r

abuild finds each upstream archive in SRCDEST, beside the APKBUILD, and checks it
against the APKBUILD's sha512sums before it builds.

Linking the runtime again with a changed library (LGPL-2.1, section 6)
----------------------------------------------------------------------

Unpack hmailserver-$version.tar.gz in an alpine container of the branch above and,
from the tarball's top directory, run

  sh build/ci/packages/appimage-runtime.sh

once, which installs Alpine's packages and links appimage/runtime-<arch>. Alpine's
repository holds only the newest build of each package of a branch: to link with
exactly the versions listed above, build each from its directory here first (above)
and install what abuild made (apk add --allow-untrusted). Put the changed library's
static archive in place of Alpine's (/usr/lib/libfuse3.a, say) and run it again as

  HM_RUNTIME_RECORD=0 sh build/ci/packages/appimage-runtime.sh

(without the record of Alpine's packages, which no longer holds). An AppImage is its
runtime followed by a SquashFS image; to put the new runtime in front of this
release's image:

  offset=\$(./hMailServer-$version-<arch>.AppImage --appimage-offset)
  tail -c +\$((offset + 1)) hMailServer-$version-<arch>.AppImage > image.squashfs
  cat appimage/runtime-<arch> image.squashfs > hMailServer-relinked.AppImage
  chmod +x hMailServer-relinked.AppImage
EOF
}

case "${1:-}" in
  record) shift; record "$@" ;;
  pack) shift; pack "$@" ;;
  *) echo "usage: $0 record MAP OUT | pack [RECORD...]" >&2; exit 2 ;;
esac
