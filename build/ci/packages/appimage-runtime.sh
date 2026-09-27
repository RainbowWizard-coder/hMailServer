#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The appimage-runtime-* jobs of .gitlab/ci/packages-appimage.yml: the AppImage
# runtime - the ELF at the front of every hMailServer AppImage - built from the
# project's own source (hmailserver/source/Server/platform/packaging/appimage/
# runtime.c) and linked statically against Alpine's packages, then proved on a
# small AppImage of its own before anything is packaged with it.
#
# Why this and not the AppImage project's runtime: that one, type2-runtime, and
# the tools that carry it (appimagetool, linuxdeploy) are published only on
# GitHub, and no distribution packages them - Debian, Ubuntu, Fedora and openSUSE
# carry libappimage and appimaged but no runtime, and Alpine's appimagetool
# downloads the runtime from GitHub when it runs (checked 26 September 2026). The
# project fetches nothing from GitHub, so it writes the runtime itself against
# the published format and links it from distribution packages:
#
#   squashfuse-dev   libsquashfuse.a and libsquashfuse_ll.a: the SquashFS reader
#                    and its FUSE low-level operations (BSD-2-Clause)
#   fuse3-static     libfuse3.a (LGPL-2.1; the runtime's source and this script
#                    are what a relink needs, and both are in this repository)
#   *-static         the decompressors squashfuse was built with: zstd, lz4,
#                    xz (lzma), zlib, and lzo (from lzo-dev, which carries
#                    the archive)
#   squashfs-tools   mksquashfs, for the self-test below
#
# Run in alpine (the job's image, x86-64 or arm64), from the root of a checkout:
#
#   sh build/ci/packages/appimage-runtime.sh
#
# Leaves, for the next job:
#   appimage/runtime-<arch>          the runtime, stripped, with the format's
#                                    magic bytes "AI\002" at offset 8
#   appimage/runtime-<arch>.txt      what it was built from: the Alpine release,
#                                    each package and version, its SHA-256
set -eu

out="${HM_OUT_DIR:-appimage}"
source_file=hmailserver/source/Server/platform/packaging/appimage/runtime.c

if [ ! -f "$source_file" ]; then
  echo "Run this from the root of a checkout (no $source_file here)."
  exit 2
fi
if [ ! -f /etc/alpine-release ]; then
  echo "This builds a static musl program and expects Alpine's packages; run it in the alpine image the job names."
  exit 2
fi

arch=$(uname -m)
case "$arch" in
  x86_64|aarch64) ;;
  *) echo "The AppImages are x86_64 and aarch64 only; this is $arch."; exit 2 ;;
esac

packages="build-base linux-headers pkgconf squashfuse-dev fuse3-dev fuse3-static squashfs-tools
          zlib-static zstd-static xz-static lz4-static lzo-dev"
# shellcheck disable=SC2086
apk add --no-cache --quiet $packages
# lzo has no -static package on Alpine; lzo-dev carries liblzo2.a.

mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The libraries a static link needs: what the pkg-config files of squashfuse and
# fuse3 say, and the five decompressors Alpine's squashfuse is built with, which
# its pkg-config file does not name (found linking without them on Alpine 3.24,
# 26 September 2026: libsquashfuse's decompress.o calls ZSTD_decompress,
# LZ4_decompress_safe, lzma_stream_buffer_decode, lzo1x_decompress_safe and
# uncompress). A squashfuse built with one more fails the link here, naming it.
static_libs="$(pkg-config --static --libs squashfuse_ll squashfuse fuse3) -lzstd -llz4 -llzma -llzo2 -lz"
echo "linked with: $static_libs"

version="1"
if [ -n "${CI_COMMIT_SHORT_SHA:-}" ]; then
  version="1 ($CI_COMMIT_SHORT_SHA)"
fi

# -I the squashfuse directory: its headers include each other by bare name.
# -Wformat=2 with -Werror: a format that is not a string literal, at a printf
# call or at a call of runtime.c's die(), which is declared a printf-style
# function, fails the build (.gitlab/security-exceptions.yml accepts die()'s
# vfprintf on the strength of that).
# shellcheck disable=SC2086
cc -std=gnu11 -O2 -Wall -Wextra -Wformat=2 -Werror -Wno-unused-parameter \
   -ffunction-sections -fdata-sections \
   -DHM_RUNTIME_VERSION="\"$version\"" \
   -I/usr/include/squashfuse -I/usr/include/fuse3 \
   -static -o "$work/runtime" "$source_file" \
   -Wl,--gc-sections $static_libs -lpthread

strip --strip-all "$work/runtime"

# The AppImage type 2 magic: "AI" and the type, 2, at offset 8 of the ELF
# identification (EI_PAD), which the kernel ignores and tools that recognise an
# AppImage read.
printf 'AI\002' | dd of="$work/runtime" bs=1 seek=8 count=3 conv=notrunc 2> /dev/null

file_type=$(od -An -tx1 -j8 -N3 "$work/runtime" | tr -d ' \n')
if [ "$file_type" != "414902" ]; then
  echo "The magic bytes were not written (read $file_type at offset 8)."
  exit 1
fi
# Static: no program interpreter and no shared library needed. (musl's ldd
# answers even for a static program, so it is not the test.)
if readelf -lW "$work/runtime" | grep -q 'INTERP' || readelf -dW "$work/runtime" | grep -q 'NEEDED'; then
  echo "The runtime is dynamically linked; it must be static:"
  readelf -lW "$work/runtime" | grep -A1 INTERP || true
  readelf -dW "$work/runtime" | grep NEEDED || true
  exit 1
fi

echo ""
echo "== the runtime proves itself on an AppImage of its own"
# An AppDir whose AppRun reports the environment the format promises, its
# arguments and a chosen exit status, packed and appended exactly as
# build-appimage.sh does. Proved: the options, extraction, extract-and-run with
# the environment and the exit status carried through, and the extracted copy
# removed after the program has exited.
mkdir -p "$work/AppDir/usr/share/probe" "$work/run"
cat > "$work/AppDir/AppRun" <<'APPRUN'
#!/bin/sh
echo "APPIMAGE=$APPIMAGE"
echo "APPDIR=$APPDIR"
echo "ARGV0=$ARGV0"
echo "OWD=$OWD"
echo "ARGS=$*"
test -f "$APPDIR/usr/share/probe/payload.txt" || exit 3
ln -s nothing "$OWD/probe-was-here" 2> /dev/null || true
exit 7
APPRUN
chmod 0755 "$work/AppDir/AppRun"
echo "payload" > "$work/AppDir/usr/share/probe/payload.txt"
ln -s usr/share/probe/payload.txt "$work/AppDir/payload-link"
mksquashfs "$work/AppDir" "$work/probe.squashfs" -root-owned -noappend -comp zstd -quiet > /dev/null

# Under qemu-user (the aarch64 job on linix, which is x86-64) the kernel hands
# an arm64 ELF to qemu through binfmt_misc, whose registration for qemu-aarch64
# matches bytes 8 to 15 of the ELF header as zeros - the magic above is there,
# so the kernel does not recognise the file at all, and the shell tries to run
# it as a script. On an arm64 machine the kernel ignores those bytes. So where
# the runtime itself will not start but a copy with the three bytes cleared
# does, the proof runs on that copy, and says so; everything but the magic is
# the same bytes.
tested_runtime="$work/runtime"
if ! "$work/runtime" --appimage-version > /dev/null 2>&1; then
  cp "$work/runtime" "$work/runtime-unmarked"
  printf '\000\000\000' | dd of="$work/runtime-unmarked" bs=1 seek=8 count=3 conv=notrunc 2> /dev/null
  if "$work/runtime-unmarked" --appimage-version > /dev/null 2>&1; then
    echo "NOTE: this $arch runtime does not start here and a copy without the AppImage magic does:"
    echo "      this is qemu-user's binfmt_misc match, not the runtime. The proof below runs on that copy."
    tested_runtime="$work/runtime-unmarked"
  else
    echo "The runtime does not start here, with or without its magic bytes."
    "$work/runtime" --appimage-version || true
    exit 1
  fi
fi

cat "$tested_runtime" "$work/probe.squashfs" > "$work/run/probe.AppImage"
chmod 0755 "$work/run/probe.AppImage"

runtime_size=$(wc -c < "$work/runtime" | tr -d ' ')
offset=$("$work/run/probe.AppImage" --appimage-offset)
[ "$offset" = "$runtime_size" ] || { echo "--appimage-offset said $offset; the runtime is $runtime_size bytes."; exit 1; }
echo "--appimage-offset: $offset, the runtime's own size"

"$work/run/probe.AppImage" --appimage-version

( cd "$work/run" && ./probe.AppImage --appimage-extract > extract.log )
cmp "$work/run/squashfs-root/usr/share/probe/payload.txt" "$work/AppDir/usr/share/probe/payload.txt"
[ "$(readlink "$work/run/squashfs-root/payload-link")" = "usr/share/probe/payload.txt" ]
[ -x "$work/run/squashfs-root/AppRun" ]
echo "--appimage-extract: every file, the symbolic link and the mode"

tmp="$work/tmp"
mkdir -p "$tmp"
set +e
( cd "$work/run" && TMPDIR="$tmp" APPIMAGE_EXTRACT_AND_RUN=1 ./probe.AppImage one "two words" > env.log 2>&1 )
code=$?
set -e
cat "$work/run/env.log"
[ "$code" -eq 7 ] || { echo "The exit status was $code, not AppRun's 7."; exit 1; }
grep -qx "APPIMAGE=$work/run/probe.AppImage" "$work/run/env.log"
grep -q "^APPDIR=$tmp/appimage_extracted_" "$work/run/env.log"
grep -qx "ARGV0=./probe.AppImage" "$work/run/env.log"
grep -qx "OWD=$work/run" "$work/run/env.log"
grep -qx "ARGS=one two words" "$work/run/env.log"
# The keeper removes the copy once AppRun has exited; give it a moment.
tries=0
while [ -n "$(ls -A "$tmp")" ] && [ "$tries" -lt 50 ]; do sleep 0.1; tries=$((tries + 1)); done
if [ -n "$(ls -A "$tmp")" ]; then
  echo "The extracted copy is still there after the program exited:"
  ls -la "$tmp"
  exit 1
fi
echo "APPIMAGE_EXTRACT_AND_RUN: the environment, the arguments and the exit status carried through, and the copy removed"

set +e
( cd "$work/run" && TMPDIR="$tmp" ./probe.AppImage --appimage-extract-and-run x > flag.log 2>&1 )
code=$?
set -e
[ "$code" -eq 7 ] && grep -qx "ARGS=x" "$work/run/flag.log" || { cat "$work/run/flag.log"; echo "--appimage-extract-and-run did not run AppRun with its arguments."; exit 1; }
echo "--appimage-extract-and-run: the same"

# With neither flag the runtime mounts; a container has no /dev/fuse, so it
# falls back to extracting and says so, each line of what it says under its
# prefix. Only where there is no /dev/fuse: with one, it would mount.
if [ ! -e /dev/fuse ]; then
  set +e
  ( cd "$work/run" && TMPDIR="$tmp" ./probe.AppImage fell back > fallback.log 2> fallback.err )
  code=$?
  set -e
  if [ "$code" -ne 7 ] || ! grep -qx "ARGS=fell back" "$work/run/fallback.log" ||
     ! grep -qx "hMailServer AppImage: FUSE is not available here (no /dev/fuse, or no fusermount3), so this runs" "$work/run/fallback.err" ||
     ! grep -qx "hMailServer AppImage: from a copy extracted under $tmp instead. APPIMAGE_EXTRACT_AND_RUN=1 skips the attempt." "$work/run/fallback.err"; then
    cat "$work/run/fallback.log" "$work/run/fallback.err"
    echo "Without FUSE the runtime did not say so and extract and run instead (exit $code)."
    exit 1
  fi
  tries=0
  while [ -n "$(ls -A "$tmp")" ] && [ "$tries" -lt 50 ]; do sleep 0.1; tries=$((tries + 1)); done
  if [ -n "$(ls -A "$tmp")" ]; then
    echo "The fallback left something in $tmp after the program exited:"
    ls -la "$tmp"
    exit 1
  fi
  echo "without FUSE: said so under its prefix, extracted and ran, and removed the copy and the mount point"
else
  echo "/dev/fuse is here, so the fallback to extracting is not tried"
fi

# An extraction that fails says which path, why, under its prefix, and exits
# non-zero: squashfs-root here is a file, so nothing can be written under it.
mkdir -p "$work/blocked"
: > "$work/blocked/squashfs-root"
set +e
( cd "$work/blocked" && "$work/run/probe.AppImage" --appimage-extract > extract.log 2> extract.err )
code=$?
set -e
if [ "$code" -eq 0 ] || ! grep -q "^hMailServer AppImage: cannot extract [^ ]*: Not a directory$" "$work/blocked/extract.err"; then
  cat "$work/blocked/extract.err"
  echo "An extraction into a file did not fail with the path and the reason (exit $code)."
  exit 1
fi
echo "--appimage-extract into a file: refused, naming the path and the reason"

set +e
"$tested_runtime" --appimage-extract > "$work/bare.log" 2>&1
code=$?
set -e
[ "$code" -ne 0 ] && grep -q "runtime alone" "$work/bare.log" || { cat "$work/bare.log"; echo "The bare runtime did not refuse."; exit 1; }
echo "the runtime alone refuses, saying it is not an AppImage"

cp "$work/runtime" "$out/runtime-$arch"
{
  echo "runtime-$arch"
  echo "sha256 $(sha256sum "$work/runtime" | cut -d' ' -f1)"
  echo "version $version"
  echo "source $source_file"
  echo "built on Alpine $(cat /etc/alpine-release) ($(uname -m)), statically, from:"
  # shellcheck disable=SC2086
  apk info -v 2> /dev/null | grep -E '^(musl|squashfuse|fuse3|zlib|zstd|xz|lz4|lzo)(-[a-z]+)?-[0-9]' | sort | sed 's/^/  /'
  echo "licences: runtime.c AGPL-3.0-or-later; squashfuse BSD-2-Clause; libfuse3 LGPL-2.1-only;"
  echo "  musl MIT; zlib Zlib; zstd BSD-3-Clause; xz 0BSD; lz4 BSD-2-Clause; lzo GPL-2.0-or-later"
} > "$out/runtime-$arch.txt"
cat "$out/runtime-$arch.txt"
