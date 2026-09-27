#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# What a pair of jobs of .gitlab/ci/packages-deb.yml does, run by hand with
# docker on an x86-64 machine: deb-build.sh in the release's own amd64
# container, then deb-install.sh in a clean container of the release and the
# package's architecture. The images are the ones the jobs name, by the same
# digests (read from .gitlab/ci/packages-deb.yml, so the two cannot drift).
#
#   bash build/ci/packages/deb-local.sh debian13 arm64
#
# from the root of a checkout (a copy: the build writes packages-deb/ and
# .cache/boost into it). An arm64 install needs the host's qemu-aarch64 binfmt
# handler with the F flag - Debian and Ubuntu's qemu-user-binfmt package, or
# docker run --privileged --rm tonistiigi/binfmt --install arm64 - and this says
# so rather than failing inside the container.
set -euo pipefail

distro="${1:-}"
arch="${2:-}"
if [ -z "$distro" ] || [ -z "$arch" ]; then
  echo "usage: $0 debian12|debian13|ubuntu24.04|ubuntu26.04 amd64|arm64"
  exit 64
fi
ci=.gitlab/ci/packages-deb.yml
[ -f "$ci" ] || { echo "run this from the root of a checkout (no $ci)"; exit 2; }

# The images the jobs' templates name: .deb-image-<distro>-<arch> in the CI file.
# The build always runs in the amd64 one (arm64 is cross-compiled there); the
# install test in the package's own architecture's.
image_of() {
  # The CI file checks out CRLF (.gitattributes), so the carriage return goes first.
  awk -v t=".deb-image-$1:" '{ sub(/\r$/, "") } $1 == t { found = 1; next } found && $1 == "name:" { print $2; exit }' "$ci"
}
build_image=$(image_of "$distro-amd64")
install_image=$(image_of "$distro-$arch")
if [ -z "$build_image" ] || [ -z "$install_image" ]; then
  echo "no .deb-image-$distro-amd64 or .deb-image-$distro-$arch template with an image name in $ci"
  exit 2
fi
echo "build in $build_image"
echo "install in $install_image"

if [ "$arch" = arm64 ] && [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
  echo "REFUSED: no qemu-aarch64 binfmt handler on this host, so no arm64 container can run the install test."
  exit 2
fi

# The containers run as root and write into the checkout; hand what they wrote
# back to whoever ran this, whatever happens.
give_back() {
  docker run --rm -v "$PWD":/src "$build_image" chown -R "$(id -u):$(id -g)" /src/packages-deb /src/.cache > /dev/null 2>&1 || true
}
trap give_back EXIT

build_started=$(date +%s)
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src \
  -e HM_DEB_DISTRO="$distro" -e HM_DEB_ARCH="$arch" -e HM_BUILD_JOBS="${HM_BUILD_JOBS:-6}" \
  "$build_image" bash build/ci/packages/deb-build.sh
echo "BUILD JOB: $(( $(date +%s) - build_started )) s"

install_started=$(date +%s)
docker run --rm --platform "linux/$arch" -v "$PWD":/src -w /src \
  -e HM_DEB_DISTRO="$distro" -e HM_DEB_ARCH="$arch" \
  "$install_image" bash build/ci/packages/deb-install.sh
echo "INSTALL JOB: $(( $(date +%s) - install_started )) s"
