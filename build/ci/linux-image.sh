#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The container image of hmailserver/source/Server/platform/packaging/docker/Dockerfile,
# for one platform, built with kaniko and checked, and never pushed from here:
#
#   sh build/ci/linux-image.sh
#       linux-image (.gitlab-ci.yml): linux/amd64, from the .deb linux-build made
#       (linux-build/packages/hmailserver_*_amd64.deb)
#   HM_IMAGE_ARCH=arm64 sh build/ci/linux-image.sh
#       linux-image-arm64 (.gitlab/ci/container-image.yml): linux/arm64, from the .deb
#       deb-ubuntu26.04-arm64 made (packages-deb/ubuntu26.04-arm64/hmailserver_*_arm64.deb)
#   HM_IMAGE_RELEASE=1 HM_IMAGE_ARCH=<amd64|arm64> sh build/ci/linux-image.sh
#       container-image-amd64 and container-image-arm64 (.gitlab/ci/container-image.yml):
#       a v* tag's image for that platform, from the .deb of the tag's version and that
#       architecture as container-image-deb took it from the published release and held
#       it to its Sigstore bundle (HM_IMAGE_DEB_DIR). Only while the project's CI/CD
#       variable HM_PUBLISH_IMAGE is 1, which is checked here as the job runs, not when
#       the pipeline is made, and only in a pipeline of the group's path, whose registry
#       is registry.gitlab.com/hmailserver/hmailserver (hmailserver/docs/ContainerImage.md).
#       The image's tarball is kept for container-image, which pushes both platforms'
#       images by digest and the one multi-platform index that names them, with the
#       release's tags (build/ci/container-image-push.sh).
#
# HM_IMAGE_DEB_DIR names another folder to take the .deb from; HM_OUT_DIR another
# folder to leave the results in (linux-image, linux-image-arm64 and
# container-image-<arch> by default).
#
# Built with kaniko, in kaniko's own image (gcr.io/kaniko-project/executor, the
# debug variant, whose busybox runs this script): a job on linix has no Docker
# daemon and no socket, and the runner is not privileged, which is what kaniko
# is for - it builds the image's filesystem inside its own container and writes
# the result to a tarball, and pushes nothing (--no-push). POSIX sh for the same
# reason: there is no bash in that image.
#
# The platform is kaniko's --custom-platform, and TARGETARCH the Dockerfile's build
# argument that picks the .deb of that architecture. The base image is pinned by the
# digest of Ubuntu's multi-platform index, and kaniko takes that platform's entry. An
# arm64 image is built on linix, an x86-64 machine: kaniko runs the Dockerfile's RUN
# in the arm64 filesystem, and linix's kernel hands each arm64 program to qemu-user
# through its binfmt_misc handler, qemu-aarch64, registered with the F flag, which
# opens the emulator when it is registered, so that it runs in any container without
# being in it and without the container being privileged. The RUN ends with
# `hmailserver --version`, so the server has run on the image's platform, with the
# image's libraries, before the image exists; its answer is kept (version.txt).
#
# What it checks in the tarball, because nothing can run the image here (that needs
# a daemon): the image is for linux/<arch>, runs as hmailserver, starts through the
# entrypoint, exposes the mail ports and the REST port, carries the healthcheck,
# HM_INSTALL_KIND and the labels a registry and Artifact Hub read, and
# /usr/bin/hmailserver keeps the one capability the Dockerfile gives it with setcap
# - a builder that dropped the file's extended attributes would ship a server that
# cannot bind port 25 as the user it runs as.
#
# A release's tarball is what container-image pushes: it holds the tarball to the
# SHA-256 this job recorded, and the manifest it pushes to the configuration this
# job checked (image.json), so what is pushed is what was checked here, and nothing
# is pushed before every check of both platforms has passed.
#
# Leaves <out>/image.json (the image's configuration), <out>/version.txt (what
# `hmailserver --version` answered in the build) and <out>/SHA256SUMS.txt (the
# tarball's and image.json's). The tarball is removed at the end, but for a
# release's build, which keeps it for container-image, and with HM_KEEP_IMAGE=1
# (`docker load -i <out>/hmailserver-image.tar`).
set -eu

arch="${HM_IMAGE_ARCH:-amd64}"
release="${HM_IMAGE_RELEASE:-0}"
context=hmailserver/source/Server/platform/packaging/docker
executor="${HM_KANIKO:-/kaniko/executor}"

case "$arch" in
  amd64|arm64) ;;
  *)
    echo "REFUSED: HM_IMAGE_ARCH is '$arch'. The image is built for amd64 and arm64, the two"
    echo "architectures a release carries a .deb for."
    exit 2
    ;;
esac
if [ "$release" = 1 ]; then
  out="${HM_OUT_DIR:-container-image-$arch}"
elif [ "$arch" = amd64 ]; then
  out="${HM_OUT_DIR:-linux-image}"
else
  out="${HM_OUT_DIR:-linux-image-$arch}"
fi

fail() {
  echo "FAILED (linux/$arch): $*"
  exit 1
}

if [ "$release" = 1 ]; then
  # The switch, read as the job runs: the jobs exist in every v* tag's pipeline, and
  # nothing is built for the registry until the project's variable says so.
  if [ "${HM_PUBLISH_IMAGE:-}" != 1 ]; then
    echo "REFUSED: the project's CI/CD variable HM_PUBLISH_IMAGE is not 1, so no image is pushed."
    echo "It is set by the maintainers for the image at registry.gitlab.com/hmailserver/hmailserver"
    echo "(hmailserver/docs/ContainerImage.md, 'Publishing'); then run the image jobs again."
    exit 3
  fi
  # A release's image: the .deb of the tag's version and this architecture, from the
  # verified release files and nowhere else. The names are the two a release carries
  # (linux-build's amd64 one and deb-ubuntu26.04-arm64's), never a
  # distribution-suffixed one.
  tag="${CI_COMMIT_TAG:-}"
  case "$tag" in
    v[0-9]*) ;;
    *) fail "HM_IMAGE_RELEASE=1 is for a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
  esac
  deb="${HM_IMAGE_DEB_DIR:-container-image-deb}/hmailserver_${tag#v}_${arch}.deb"
  if [ ! -f "$deb" ]; then
    echo "REFUSED: no $deb. container-image-deb's artifact holds the release's .deb for each"
    echo "architecture, taken from the published release and held to its Sigstore bundle; it did not arrive."
    exit 2
  fi
  # Where the image goes: the project's own repository at the group's path, the
  # image's lasting address, which the Compose file and the chart name, and never
  # anywhere else. container-image pushes (build/ci/container-image-push.sh), and
  # holds the path again; nothing is built for another. A container repository's path
  # is always its project's, and the maintainers decided on 27 September 2026 to push
  # the first image only once the project was in the group, so that its address would
  # not change with the move. The project moved on 29 September 2026; a pipeline whose
  # registry is the first path, registry.gitlab.com/progressiverobot/hmailserver, is
  # another project's now.
  repository="${CI_REGISTRY_IMAGE:-}"
  case "$repository" in
    registry.gitlab.com/hmailserver/hmailserver) ;;
    *) fail "the project's registry image is '$repository', not registry.gitlab.com/hmailserver/hmailserver, which the Compose file and the chart name" ;;
  esac
else
  case "$arch" in
    amd64) made_by=linux-build; debdir="${HM_IMAGE_DEB_DIR:-linux-build/packages}" ;;
    *) made_by="deb-ubuntu26.04-arm64"; debdir="${HM_IMAGE_DEB_DIR:-packages-deb/ubuntu26.04-arm64}" ;;
  esac
  deb=$(ls "$debdir"/hmailserver_*_"$arch".deb 2> /dev/null | head -n 1 || true)
  if [ -z "$deb" ]; then
    echo "REFUSED: no $debdir/hmailserver_*_$arch.deb. The job $made_by made none; its artifact says why."
    exit 2
  fi
fi
version=$(basename "$deb" | sed "s/^hmailserver_\(.*\)_${arch}\.deb\$/\1/")
echo "platform: linux/$arch"
echo "package:  $deb (version $version)"
# On a release tag the package must be the tag's version, as the old job checked.
case "${CI_COMMIT_TAG:-}" in
  v*) [ "${CI_COMMIT_TAG#v}" = "$version" ] || fail "the .deb is $version, and the tag is $CI_COMMIT_TAG" ;;
esac

# Only the one .deb goes into the context: the .rpm is not for the image, and the
# other architecture's .deb is not for this one.
rm -rf "$context/packages" "$out"
mkdir -p "$context/packages" "$out"
cp "$deb" "$context/packages/"

# The labels that change with the build; the Dockerfile has the rest. created is the
# commit's time, so that two builds of one commit say the same (a build outside CI has
# no commit time, and says when it was made). Artifact Hub reads created, and
# prerelease for a version with a suffix.
created="${CI_COMMIT_TIMESTAMP:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}"
case "$version" in
  *[!0-9.]*) prerelease=true ;;
  *) prerelease=false ;;
esac

"$executor" version 2> /dev/null || true
# The build's output goes to the job's log and to a file, for the version check below,
# and kaniko's exit status to a file of its own, since a pipeline's status is its last
# command's. HM_KANIKO_FLAGS is for a test of one's own; the jobs never set it. It is
# split into words on purpose: busybox sh has no arrays.
{
  status=0
  # shellcheck disable=SC2086
  # nosemgrep
  "$executor" \
    --context "dir://$PWD/$context" \
    --dockerfile "$PWD/$context/Dockerfile" \
    --custom-platform "linux/$arch" \
    --build-arg "TARGETARCH=$arch" \
    --no-push --destination "hmailserver:$version-$arch" ${HM_KANIKO_FLAGS:-} \
    --tar-path "$out/hmailserver-image.tar" \
    --label "org.opencontainers.image.version=$version" \
    --label "org.opencontainers.image.revision=${CI_COMMIT_SHA:-unknown}" \
    --label "org.opencontainers.image.created=$created" \
    --label "io.artifacthub.package.prerelease=$prerelease" \
    --cleanup 2>&1 || status=$?
  echo "$status" > "$out/kaniko.status"
} | tee "$out/kaniko.log"
status=$(cat "$out/kaniko.status" 2> /dev/null || echo 1)
rm -f "$out/kaniko.status"
rm -rf "$context/packages"
[ "$status" = 0 ] || fail "kaniko did not build the image (exit status $status)"

echo ""
echo "== what was built"
ls -la "$out"
# The server answered on the image's platform, in the build: its version line.
expected="${version%%[!0-9.]*}"
grep -F "hMailServer $expected build " "$out/kaniko.log" | head -n 1 > "$out/version.txt" || true
[ -s "$out/version.txt" ] || fail "the build did not print 'hMailServer $expected build <n>': hmailserver --version did not answer in the image"
echo "hmailserver --version in the linux/$arch build: $(cat "$out/version.txt")"
rm -f "$out/kaniko.log"

work="$out/unpacked"
mkdir -p "$work"
tar -xf "$out/hmailserver-image.tar" -C "$work"
config=$(sed -n 's/.*"Config":"\([^"]*\)".*/\1/p' "$work/manifest.json")
[ -n "$config" ] && [ -f "$work/$config" ] || fail "the tarball has no image configuration named in manifest.json"
cp "$work/$config" "$out/image.json"
tr ',' '\n' < "$out/image.json" | grep -E '"(architecture|os|variant|User|Entrypoint|WorkingDir|Healthcheck|Test)"|/tcp' | head -n 24

grep -q "\"architecture\":\"$arch\"" "$out/image.json" || fail "the image is not for architecture $arch"
grep -q '"os":"linux"' "$out/image.json" || fail "the image is not for os linux"
echo "for linux/$arch"
grep -q '"User":"hmailserver"' "$out/image.json" || fail "the image does not run as hmailserver"
grep -q 'docker-entrypoint.sh' "$out/image.json" || fail "the image does not start through docker-entrypoint.sh"
grep -q '"Healthcheck"' "$out/image.json" || fail "the image carries no healthcheck"
for port in 25 110 143 465 587 993 995 4190 8045; do
  grep -q "\"$port/tcp\"" "$out/image.json" || fail "the image does not expose $port/tcp"
done
echo "runs as hmailserver, through the entrypoint, with the healthcheck and the nine ports"
# The install kind the server's update check reads, and the three labels Artifact Hub
# will not list an image without.
grep -q '"HM_INSTALL_KIND=container"' "$out/image.json" || fail "the image does not set HM_INSTALL_KIND=container for the update check"
for label in io.artifacthub.package.readme-url org.opencontainers.image.created org.opencontainers.image.description \
             org.opencontainers.image.source org.opencontainers.image.licenses; do
  grep -q "\"$label\":\"[^\"]" "$out/image.json" || fail "the image carries no $label label"
done
grep -q "\"org.opencontainers.image.version\":\"$version\"" "$out/image.json" || fail "the image's version label is not $version"
echo "HM_INSTALL_KIND=container, and the labels a registry and Artifact Hub read, version $version"

# The layer that holds the binary, and the capability on it: tar writes a file's
# extended attributes as a SCHILY.xattr PAX record in front of it.
layers=$(sed -n 's/.*"Layers":\[\([^]]*\)\].*/\1/p' "$work/manifest.json" | tr -d '"' | tr ',' ' ')
found=""
for layer in $layers; do
  if zcat "$work/$layer" 2> /dev/null | tar -tf - 2> /dev/null | grep -qx 'usr/bin/hmailserver'; then
    found="$layer"
  fi
done
[ -n "$found" ] || fail "no layer holds usr/bin/hmailserver"
if zcat "$work/$found" | grep -c 'SCHILY.xattr.security.capability' > /dev/null; then
  echo "/usr/bin/hmailserver carries its file capability (security.capability) in layer $found"
else
  fail "/usr/bin/hmailserver lost the capability setcap gave it: the builder dropped the file's extended attributes"
fi

( cd "$out" && sha256sum hmailserver-image.tar image.json > SHA256SUMS.txt && cat SHA256SUMS.txt )
rm -rf "$work"
if [ "$release" = 1 ]; then
  echo "IMAGE BUILT: the release's linux/$arch image, $version, configuration $config; container-image pushes it with the other platform's."
  exit 0
fi
if [ "${HM_KEEP_IMAGE:-0}" != 1 ]; then
  rm -f "$out/hmailserver-image.tar"
fi
echo "IMAGE BUILT: hmailserver:$version-$arch (linux/$arch), configuration $config, not pushed."
