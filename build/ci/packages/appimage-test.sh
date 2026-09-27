#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# An hMailServer AppImage, run: by appimage-build.sh on the Ubuntu 24.04 it was
# built on, and by the appimage-test-* jobs of .gitlab/ci/packages-appimage.yml in
# a clean container of another distribution (Fedora), where nothing the build
# installed is present. From the root of a checkout:
#
#   bash build/ci/packages/appimage-test.sh [appimage/hMailServer-<v>-<arch>.AppImage]
#
# With no argument it takes the one AppImage for this machine in appimage/.
#
# What it proves:
#   * the runtime answers its own options, and the filesystem starts where the
#     runtime ends;
#   * extracted and run (APPIMAGE_EXTRACT_AND_RUN=1, and --appimage-extract-and-run)
#     the server starts on this distribution's glibc and host libraries and says
#     its version, which is the version in the file name, and AppRun has made the
#     per-user tree and its configuration;
#   * the image carries the Control Deck and every one of its catalogues byte for
#     byte, the schema scripts, the Diffie-Hellman group, the public-suffix list,
#     the live update's apply step, the licences, the desktop file and the icon;
#   * where FUSE can be had (a /dev/fuse this process may open and a fusermount3),
#     the same run mounted instead of extracted. In a container there is usually no
#     FUSE, and that is said, not failed.
set -euo pipefail

arch=$(uname -m)
image="${1:-}"
if [ -z "$image" ]; then
  image=$(find appimage -maxdepth 1 -name "hMailServer-*-$arch.AppImage" 2> /dev/null | head -n 1)
fi
if [ -z "$image" ] || [ ! -f "$image" ]; then
  echo "No AppImage for $arch (looked in appimage/)."
  exit 2
fi
image=$(cd "$(dirname "$image")" && pwd)/$(basename "$image")
chmod +x "$image"
expected=$(basename "$image" | sed -n "s/^hMailServer-\(.*\)-$arch\.AppImage$/\1/p")
if [ -z "$expected" ]; then
  echo "$(basename "$image") is not named hMailServer-<version>-$arch.AppImage, and the version it should say is read from that name."
  exit 2
fi

. /etc/os-release
echo "$(basename "$image") on $PRETTY_NAME, $(ldd --version 2>&1 | head -n 1)"

# --version prints "hMailServer <version> build <n>"; the file name carries the version.
says_version()
{
  case "$1" in
    "hMailServer $expected" | "hMailServer $expected "*) return 0 ;;
  esac
  return 1
}

# Byte for byte, with coreutils alone: a minimal Fedora has no cmp (diffutils).
same_bytes()
{
  [ "$(sha256sum < "$1")" = "$(sha256sum < "$2")" ] || { echo "$1 and $2 differ."; exit 1; }
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Under qemu-user (an arm64 container on an x86-64 host) binfmt_misc does not
# recognise an ELF whose bytes 8 to 10 carry the AppImage magic - its match for
# qemu-aarch64 wants zeros there - and the file cannot be started at all. On an
# arm64 machine it can. Where that is what happens, the tests run on a copy with
# those three bytes cleared, and say so; every other byte is the AppImage's.
if ! "$image" --appimage-version > /dev/null 2>&1; then
  cp "$image" "$work/unmarked.AppImage"
  printf '\000\000\000' | dd of="$work/unmarked.AppImage" bs=1 seek=8 count=3 conv=notrunc 2> /dev/null
  chmod +x "$work/unmarked.AppImage"
  if "$work/unmarked.AppImage" --appimage-version > /dev/null 2>&1; then
    echo "NOTE: $(basename "$image") does not start here and a copy without the AppImage magic does:"
    echo "      qemu-user's binfmt_misc match, not the AppImage. The tests below run on that copy."
    mkdir -p "$work/bin"
    image="$work/bin/$(basename "$image")"
    mv "$work/unmarked.AppImage" "$image"
  else
    "$image" --appimage-version || true
    echo "$(basename "$image") does not start here, with or without its magic bytes."
    exit 1
  fi
fi

"$image" --appimage-version
offset=$("$image" --appimage-offset)
[ "$offset" -gt 0 ] && [ "$offset" -lt "$(stat -c %s "$image")" ] || { echo "--appimage-offset said $offset."; exit 1; }
# A SquashFS 4 superblock begins "hsqs".
[ "$(dd if="$image" bs=1 skip="$offset" count=4 2> /dev/null)" = "hsqs" ] \
  || { echo "No SquashFS at offset $offset."; exit 1; }
echo "the filesystem begins at $offset, where the runtime ends"

echo ""
echo "== extracted and run"
home="$work/home"
said=$(TMPDIR="$work" HMAILSERVER_HOME="$home" APPIMAGE_EXTRACT_AND_RUN=1 "$image" --version)
echo "$said"
says_version "$said" || { echo "It said \"$said\", and the file name says $expected."; exit 1; }
[ -f "$home/hMailServer.ini" ] || { echo "AppRun made no configuration at $home/hMailServer.ini."; exit 1; }
[ "$(stat -c %a "$home/hMailServer.ini")" = "600" ] || { echo "The configuration is not 0600."; exit 1; }
grep -q "^ProgramFolder=$work/appimage_extracted_" "$home/hMailServer.ini" \
  || { echo "ProgramFolder does not point into the running image:"; grep '^ProgramFolder=' "$home/hMailServer.ini"; exit 1; }
for d in data logs temp events database; do
  [ -d "$home/$d" ] || { echo "AppRun did not make $home/$d."; exit 1; }
done
TMPDIR="$work" HMAILSERVER_HOME="$home" "$image" --appimage-extract-and-run --help > "$work/help.txt"
grep -q -- "--check-config" "$work/help.txt" || { cat "$work/help.txt"; echo "--help did not list --check-config."; exit 1; }
echo "--version and --help answer; AppRun made the per-user tree and a 0600 configuration"
# The copy is removed once the server has exited.
tries=0
while ls -d "$work"/appimage_extracted_* > /dev/null 2>&1 && [ "$tries" -lt 50 ]; do sleep 0.1; tries=$((tries + 1)); done
if ls -d "$work"/appimage_extracted_* > /dev/null 2>&1; then
  echo "An extracted copy is still in $work after the server exited."
  exit 1
fi
echo "and the extracted copies are gone"
# Informational: with no database configured, what --check-config says.
set +e
TMPDIR="$work" HMAILSERVER_HOME="$home" APPIMAGE_EXTRACT_AND_RUN=1 "$image" --check-config > "$work/check.txt" 2>&1
code=$?
set -e
echo "--check-config with no database configured: exit $code, \"$(head -n 1 "$work/check.txt")\""

echo ""
echo "== what the image carries"
( cd "$work" && "$image" --appimage-extract > /dev/null )
root="$work/squashfs-root"
for f in AppRun hmailserver.desktop hmailserver.svg .DirIcon \
         usr/bin/hmailserver usr/bin/dh2048.pem usr/bin/tlds.txt \
         usr/share/hmailserver/DBScripts/CreateTablesPGSQL.sql \
         usr/share/hmailserver/WebAdmin/index.html \
         usr/share/hmailserver/hMailServer.ini.default \
         usr/share/hmailserver/licenses/QuickJS.txt \
         usr/share/hmailserver/licenses/appimage-runtime.txt \
         usr/lib/hmailserver/hmailserver-update; do
  [ -e "$root/$f" ] || { echo "The image has no $f."; exit 1; }
done
[ -x "$root/AppRun" ] && [ -x "$root/usr/bin/hmailserver" ] && [ -x "$root/usr/lib/hmailserver/hmailserver-update" ] \
  || { echo "AppRun, the server or the update step is not executable."; exit 1; }
if [ -f hmailserver/installation/WebAdmin/index.html ]; then
  same_bytes "$root/usr/share/hmailserver/WebAdmin/index.html" hmailserver/installation/WebAdmin/index.html
  count=0
  for catalogue in hmailserver/installation/WebAdmin/languages/*.json; do
    same_bytes "$catalogue" "$root/usr/share/hmailserver/WebAdmin/languages/$(basename "$catalogue")"
    count=$((count + 1))
  done
  echo "the Control Deck and its $count catalogues, byte for byte"
fi
libraries=0
for library in "$root"/usr/lib/*.so*; do
  if [ -f "$library" ]; then
    libraries=$((libraries + 1))
  fi
done
echo "the libraries in the image: $libraries"
readelf -d "$root/usr/bin/hmailserver" 2> /dev/null | grep -E 'RUNPATH|RPATH' || true

echo ""
echo "== mounted with FUSE"
if [ -r /dev/fuse ] && [ -w /dev/fuse ] && { command -v fusermount3 > /dev/null 2>&1 || [ -x /usr/bin/fusermount3 ]; }; then
  set +e
  said=$(TMPDIR="$work" HMAILSERVER_HOME="$home" "$image" --version 2> "$work/fuse.err")
  code=$?
  set -e
  if [ "$code" -eq 0 ] && says_version "$said" && ! grep -q "FUSE is not available" "$work/fuse.err"; then
    echo "$said - mounted, not extracted"
  else
    cat "$work/fuse.err"
    echo "FUSE is here, but the mounted run failed (exit $code, said \"$said\")."
    exit 1
  fi
else
  echo "no FUSE in this environment (no usable /dev/fuse or no fusermount3): the mounted run is not tried here"
fi

echo ""
echo "$(basename "$image") runs on $PRETTY_NAME."
