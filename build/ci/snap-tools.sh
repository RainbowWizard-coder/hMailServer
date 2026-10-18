#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# snapcraft, for snap-build and snap-upload (.gitlab/ci/snap.yml,
# .gitlab/ci/channels.yml; hmailserver/docs/Snap.md), in an Ubuntu 24.04 job
# container of the machine's own architecture, without snapd running.
#
# snapcraft is itself a snap, and the job containers have no snapd: so the two snaps
# it needs - snapcraft and core24, its base - are downloaded from the Snap Store at a
# pinned revision, held to the SHA3-384 the store's signed snap-revision assertion
# gives for that revision (written below), and unpacked where snapd would mount
# them, /snap/<name>/current, where snapcraft's own programs look for their
# interpreter and libraries. A /usr/local/bin/snapcraft then runs it as snapd would,
# with the environment snapd gives it. snapcraft packs with snapd's own snap pack,
# which needs no daemon either: that, squashfs-tools, python3, curl, git and
# openssh-client (snap-upload holds the pipeline's commit to the release tag's with
# git fetch and git verify-tag) come from Ubuntu's archive. Nothing comes from GitHub.
#
# A newer snapcraft or core24: the revision and SHA3-384 for each architecture from
# the store's answer (Snap.md, "Moving snapcraft"), and then a snap-build.
#
# Usage, as root: bash build/ci/snap-tools.sh
set -euo pipefail

# The pins: <name> <architecture> <revision> <SHA3-384, hex>. snapcraft 9.1.3 from
# 9.x/stable and core24 20260824 from latest/stable, read on 2 October 2026; each
# hash checked against the store's signed snap-revision assertion with snap ack.
PINS="
snapcraft amd64 19192 4d552cd603bdf82b75957ff5d67493c1f39cb7a8f740b3f049b832b7591c26d7db17763151bfc4e0f0f145b44cb0476e
snapcraft arm64 19189 d429e1328f387e0827727611e735bf6ef6a3ec5bb51e92a9167e34dcdff8bfb4091aaa5052e27f389e346b9ec9b1d355
core24 amd64 2124 9f84f391c5ace85b755a7530a330f7b52aff891178cdfdf10bc4f673f261ea9454629ddd933991cbedaf7963e689789c
core24 arm64 2125 43eb1ae091333a763cdebc9b9788a1b99d24c474bb95f12877d15ccda429b50689ed19ce7e6a7a2d24e00035ae486b2c
"
# The store's ids for the two names, which its download address is made of.
declare -A SNAP_ID=(
   [snapcraft]=vMTKRaLjnOJQetI78HjntT37VuoyssFE
   [core24]=dwTAh7MZZ01zyriOZErqd1JynQLiOGvM
)
STORE_DOWNLOAD=https://api.snapcraft.io/api/v1/snaps/download

say() { printf 'snap-tools: %s\n' "$*"; }
fail() { printf 'snap-tools: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root: it installs packages and writes /snap and /usr/local/bin"
. /etc/os-release
[ "${VERSION_ID:-}" = "24.04" ] || fail "this is ${PRETTY_NAME:-an unknown system}; snapcraft builds a core24 snap in destructive mode on Ubuntu 24.04 only"

arch="$(dpkg --print-architecture)"
case "$arch" in
   amd64|arm64) ;;
   *) fail "no pinned snapcraft for $arch" ;;
esac

say "squashfs-tools, snapd (for snap pack), python3, curl, git and openssh-client (for snap-upload's check of the tag and its signature) from Ubuntu's archive"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq -o Acquire::Retries=3 --no-install-recommends \
   ca-certificates curl git openssh-client python3 squashfs-tools snapd sudo > /dev/null

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fetch() {
   local name="$1" revision="$2" want="$3" file="$work/$1.snap" got
   say "$name revision $revision ($arch) from the Snap Store"
   curl -fsSL --retry 3 --proto '=https' --tlsv1.2 -o "$file" "$STORE_DOWNLOAD/${SNAP_ID[$name]}_$revision.snap"
   got="$(python3 -c 'import hashlib, sys; h = hashlib.sha3_384(); f = open(sys.argv[1], "rb"); [h.update(b) for b in iter(lambda: f.read(1 << 20), b"")]; print(h.hexdigest())' "$file")"
   if [ "$got" != "$want" ]; then
      fail "$name revision $revision has SHA3-384 $got, not the pinned $want: not used"
   fi
   say "$name revision $revision: SHA3-384 as pinned"
   rm -rf "/snap/$name"
   mkdir -p "/snap/$name"
   unsquashfs -q -n -d "/snap/$name/$revision" "$file" > /dev/null
   ln -sfn "$revision" "/snap/$name/current"
   rm -f "$file"
}

found=0
while read -r name pin_arch revision sha; do
   [ -n "$name" ] || continue
   [ "$pin_arch" = "$arch" ] || continue
   fetch "$name" "$revision" "$sha"
   found=$((found + 1))
done <<< "$PINS"
[ "$found" = 2 ] || fail "the pins name $found snaps for $arch, not 2"

grep -qx 'name: snapcraft' /snap/snapcraft/current/meta/snap.yaml || fail "the snapcraft snap's meta/snap.yaml does not name snapcraft"
version="$(sed -n 's/^version: //p' /snap/snapcraft/current/meta/snap.yaml | tr -d "'\"")"

# What snapd would set for the app snapcraft.snapcraft (its meta/snap.yaml): the
# snap's directories and its own PATH first. Not SNAP_NAME: with it, snapcraft takes
# itself for a snap snapd started and asks snapd for its own snap configuration
# (craft_application's SnapConfigHandler, through snapctl), and there is no snapd.
cat > /usr/local/bin/snapcraft <<EOF
#!/bin/sh
export SNAP=/snap/snapcraft/current
unset SNAP_NAME SNAP_INSTANCE_NAME
export SNAP_VERSION='$version'
export SNAP_REVISION="\$(readlink /snap/snapcraft/current)"
export SNAP_ARCH='$arch'
export SNAP_USER_DATA=/root/snap/snapcraft/current
export SNAP_USER_COMMON=/root/snap/snapcraft/common
export SNAP_DATA=/var/snap/snapcraft/current
export SNAP_COMMON=/var/snap/snapcraft/common
export PATH="\$SNAP/libexec/snapcraft:/snap/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export CRYPTOGRAPHY_OPENSSL_NO_LEGACY=1
export PYLXD_WARNINGS=none
mkdir -p "\$SNAP_USER_DATA" "\$SNAP_USER_COMMON"
exec "\$SNAP/bin/python" "\$SNAP/bin/snapcraft" "\$@"
EOF
chmod 0755 /usr/local/bin/snapcraft

snapcraft --version
say "snapcraft $version on core24 $(readlink /snap/core24/current), $arch: /usr/local/bin/snapcraft"
