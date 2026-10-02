#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The snap (snap/snapcraft.yaml; hmailserver/docs/Snap.md), built from the Ubuntu
# 24.04 .deb of the same pipeline, in an Ubuntu 24.04 job container of the snap's own
# architecture: the jobs snap-build-amd64 and snap-build-arm64 (.gitlab/ci/snap.yml).
#
#   1. snapcraft, pinned, from the Snap Store (build/ci/snap-tools.sh);
#   2. the .deb - deb-ubuntu24.04-<arch>'s hmailserver_<v>-1~ubuntu24.04_<arch>.deb,
#      or the file HM_SNAP_DEB names - put where snapcraft.yaml's part reads it;
#   3. snapcraft pack in destructive mode, here, for this architecture - no LXD, no
#      remote build: core24 is Ubuntu 24.04, which this container is;
#   4. the snap held to what it must be: its meta/snap.yaml's name, version (the
#      .deb's, without the Debian revision), architecture, base and strict
#      confinement; the files it must carry and the ones it must not (the systemd
#      units, the live update's helper); the server it carries started with the
#      snap's libraries and answering --version with that version;
#   5. snap/hmailserver_<version>_<arch>.snap, which snap-upload takes
#      (build/ci/channels/snap-upload.py's contract), and in snap-build/<arch>/ the
#      snap's meta/snap.yaml, SHA-256 and SHA3-384, its file list and snapcraft's
#      log. (snapcraft writes no manifest.yaml: that needs SNAPCRAFT_BUILD_INFO, which
#      would put one into the snap.)
#
# No .snap.source is written: the snap is this pipeline's commit's, which
# snap-upload holds to the release tag's.
#
# Usage, as root, from the root of a checkout: bash build/ci/snap-build.sh
set -euo pipefail

say() { printf 'snap-build: %s\n' "$*"; }
fail() { printf 'snap-build: FAIL %s\n' "$*" >&2; exit 1; }

root="$(pwd)"
[ -f snap/snapcraft.yaml ] || fail "run from the root of a checkout (no snap/snapcraft.yaml here)"

bash build/ci/snap-tools.sh
arch="$(dpkg --print-architecture)"

deb="${HM_SNAP_DEB:-}"
if [ -z "$deb" ]; then
   shopt -s nullglob
   candidates=(packages-deb/ubuntu24.04-"$arch"/hmailserver_*~ubuntu24.04_"$arch".deb)
   shopt -u nullglob
   [ "${#candidates[@]}" = 1 ] || fail "packages-deb/ubuntu24.04-$arch holds ${#candidates[@]} .deb files of this architecture, not one (deb-ubuntu24.04-$arch's artifacts)"
   deb="${candidates[0]}"
fi
[ -f "$deb" ] || fail "no .deb at $deb"
[ "$(dpkg-deb -f "$deb" Package)" = hmailserver ] || fail "$deb is not the hmailserver package"
[ "$(dpkg-deb -f "$deb" Architecture)" = "$arch" ] || fail "$deb is $(dpkg-deb -f "$deb" Architecture), and this container is $arch"
deb_version="$(dpkg-deb -f "$deb" Version)"
version="${deb_version%%-*}"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "$deb's version $deb_version is not <major>.<minor>.<patch>-<revision>"
say "the server from $deb ($deb_version)"

rm -rf snap-build/input parts stage prime
mkdir -p snap-build/input "snap-build/$arch"
cp "$deb" "snap-build/input/hmailserver_$arch.deb"

say "snapcraft pack, destructive mode, for $arch"
started=$SECONDS
if ! snapcraft pack --destructive-mode --platform "$arch" --verbosity brief > "snap-build/$arch/snapcraft.log" 2>&1; then
   tail -n 60 "snap-build/$arch/snapcraft.log" >&2
   fail "snapcraft pack failed (snap-build/$arch/snapcraft.log)"
fi
say "packed in $((SECONDS - started)) s"
grep -iE 'lint|warning' "snap-build/$arch/snapcraft.log" || true

built="hmailserver_${version}_$arch.snap"
[ -f "$built" ] || fail "snapcraft made no $built: $(printf '%s ' ./*.snap)"

# --- what the snap is ---------------------------------------------------------
meta="$(unsquashfs -cat "$built" meta/snap.yaml)"
printf '%s\n' "$meta" > "snap-build/$arch/snap.yaml"
field() { printf '%s\n' "$meta" | sed -n "s/^$1: //p" | head -n 1 | tr -d "'\""; }
[ "$(field name)" = hmailserver ] || fail "meta/snap.yaml names $(field name), not hmailserver"
[ "$(field version)" = "$version" ] || fail "meta/snap.yaml's version is $(field version), not the .deb's $version"
[ "$(field base)" = core24 ] || fail "meta/snap.yaml's base is $(field base), not core24"
[ "$(field confinement)" = strict ] || fail "meta/snap.yaml's confinement is $(field confinement), not strict"
[ "$(field grade)" = stable ] || fail "meta/snap.yaml's grade is $(field grade), not stable"
printf '%s\n' "$meta" | grep -qx -- "- $arch" || fail "meta/snap.yaml's architectures are not [$arch]"
printf '%s\n' "$meta" | grep -qx '  snap_daemon: shared' || fail "meta/snap.yaml does not ask for the snap_daemon account"
say "meta/snap.yaml: hmailserver $version, $arch, core24, strict, stable, snap_daemon"

unsquashfs -l "$built" | sed 's|^squashfs-root||' | sort > "snap-build/$arch/files.txt"
for must in /usr/bin/hmailserver /usr/bin/dh2048.pem /usr/bin/hmctl /bin/hmailserver-daemon /bin/hmailserver-start \
            /bin/hmailserver-run /bin/hmailserver-cli /bin/hmailserver-snap-env /bin/hmailserver-snap-config \
            /bin/hmailserver-set-database-password /bin/hmailserver-entrypoint /meta/hooks/configure \
            /etc/hmailserver/hMailServer.ini /usr/share/hmailserver/WebAdmin/index.html /usr/bin/timeout \
            /usr/share/hmailserver/DBScripts/CreateTablesPGSQL.sql /usr/share/hmailserver/licenses/AGPL-3.0.txt; do
   grep -qx -- "$must" "snap-build/$arch/files.txt" || fail "the snap has no $must"
done
for never in /usr/lib/systemd /usr/lib/hmailserver /etc/fail2ban /etc/logrotate.d; do
   if grep -q -- "^$never" "snap-build/$arch/files.txt"; then
      fail "the snap carries $never, which a snap does not use"
   fi
done
say "the snap's files: what it must carry is there, the units and the live update's helper are not ($(wc -l < "snap-build/$arch/files.txt") entries)"

# The server, with the snap's libraries over core24's (this container's are core24's
# own release): every library found, and --version the snap's.
case "$arch" in amd64) triplet=x86_64-linux-gnu ;; arm64) triplet=aarch64-linux-gnu ;; esac
missing="$(LD_LIBRARY_PATH="$root/prime/usr/lib/$triplet:$root/prime/usr/lib" ldd prime/usr/bin/hmailserver | grep 'not found' || true)"
[ -z "$missing" ] || fail "the server's libraries are not all in the snap or core24: $missing"
said="$(LD_LIBRARY_PATH="$root/prime/usr/lib/$triplet:$root/prime/usr/lib" prime/usr/bin/hmailserver --version 2>&1 | head -n 1)"
case "$said" in
   *"$version"*) say "prime/usr/bin/hmailserver --version: $said" ;;
   *) fail "the snap's server says '$said', not version $version" ;;
esac
python3 prime/bin/hmailserver-snap-config --self-test --source "$root"

mv -f "$built" "snap/$built"
(
   cd snap
   sha256sum "$built" > "../snap-build/$arch/SHA256SUMS.txt"
   python3 -c 'import hashlib, sys; h = hashlib.sha3_384(); f = open(sys.argv[1], "rb"); [h.update(b) for b in iter(lambda: f.read(1 << 20), b"")]; print(h.hexdigest() + "  " + sys.argv[1])' "$built" > "../snap-build/$arch/SHA3-384SUMS.txt"
)
rm -rf snap-build/input parts stage prime
say "PASS snap/$built ($(stat -c %s "snap/$built") bytes, SHA-256 $(cut -d' ' -f1 "snap-build/$arch/SHA256SUMS.txt"))"
