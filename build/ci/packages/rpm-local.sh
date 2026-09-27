#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# What a pair of jobs of .gitlab/ci/packages-rpm.yml does, run by hand with
# docker on an x86-64 machine: rpm-build.sh in the distribution's own x86_64
# container, then rpm-install-test.sh in a clean container of the distribution
# and the package's architecture. The images are the ones the jobs name, by the
# same digests (read from .gitlab/ci/packages-rpm.yml, so the two cannot drift).
#
#   bash build/ci/packages/rpm-local.sh el10 aarch64
#
# from the root of a checkout (a copy: the build writes packages-rpm/ into it).
# An aarch64 install needs the host's qemu-aarch64 binfmt handler with the F
# flag - Ubuntu's and Debian's qemu-user-binfmt package, or
# docker run --privileged --rm tonistiigi/binfmt --install arm64 - and this says
# so rather than failing inside the container.
set -euo pipefail

target="${1:-}"
arch="${2:-}"
if [ -z "$target" ] || [ -z "$arch" ]; then
  echo "usage: $0 fedora44|fedora43|el10|el9|leap16|tumbleweed x86_64|aarch64"
  exit 64
fi
ci=.gitlab/ci/packages-rpm.yml
[ -f "$ci" ] || { echo "run this from the root of a checkout (no $ci)"; exit 2; }

# The images the jobs' templates name: .rpm-image-<target>-<arch> in the CI file.
# The build always runs in the x86_64 one (aarch64 is cross-compiled there); the
# install test in the package's own architecture's.
image_of() {
  # The CI file checks out CRLF (.gitattributes), so the carriage return goes first.
  awk -v t=".rpm-image-$1:" '{ sub(/\r$/, "") } $1 == t { found = 1; next } found && $1 == "name:" { print $2; exit }' "$ci"
}
build_image=$(image_of "$target-x86_64")
install_image=$(image_of "$target-$arch")
if [ -z "$build_image" ] || [ -z "$install_image" ]; then
  echo "no .rpm-image-$target-x86_64 or .rpm-image-$target-$arch template with an image name in $ci"
  exit 2
fi
platform=linux/amd64
[ "$arch" = aarch64 ] && platform=linux/arm64
echo "build in $build_image"
echo "install in $install_image ($platform)"

if [ "$arch" = aarch64 ] && [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
  echo "REFUSED: no qemu-aarch64 binfmt handler on this host, so no aarch64 container can run the install test."
  exit 2
fi

# The containers run as root and write into the checkout; hand what they wrote
# back to whoever ran this, whatever happens.
give_back() {
  docker run --rm --platform linux/amd64 -v "$PWD":/src "$build_image" chown -R "$(id -u):$(id -g)" /src/packages-rpm > /dev/null 2>&1 || true
}
trap give_back EXIT

build_started=$(date +%s)
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src \
  -e HM_BUILD_JOBS="${HM_BUILD_JOBS:-6}" \
  "$build_image" bash build/ci/packages/rpm-build.sh "$target" "$arch"
echo "BUILD JOB: $(( $(date +%s) - build_started )) s"

install_started=$(date +%s)
docker run --rm --platform "$platform" -v "$PWD":/src -w /src \
  "$install_image" bash build/ci/packages/rpm-install-test.sh "$target" "$arch"
echo "INSTALL JOB: $(( $(date +%s) - install_started )) s"
