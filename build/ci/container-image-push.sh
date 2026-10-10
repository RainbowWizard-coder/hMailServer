#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The container-image job of .gitlab/ci/container-image.yml: the release's image for
# linux/amd64 and for linux/arm64, which container-image-amd64 and container-image-arm64
# built and checked (build/ci/linux-image.sh with HM_IMAGE_RELEASE=1), pushed to the
# project's container registry each by its digest, under one multi-platform index -
# a Docker manifest list naming both - which carries the release's tags: :<version>,
# and for a release without a suffix :<major>.<minor> and :latest too, but only forward.
# A pre-release (v6.3.6-rc1) never moves those two. Run in regctl's own image (docker.io's
# regclient/regctl, the -alpine variant of the release HM_REGCTL_IMAGE pins in
# .gitlab/ci/channels.yml, whose busybox is the shell), from the root of the checkout:
#
#   sh build/ci/container-image-push.sh
#
# In:   container-image-<arch>/hmailserver-image.tar, image.json and SHA256SUMS.txt for
#       amd64 and arm64, the two platform jobs' artifacts.
# Out:  container-image/image.env, a dotenv report for the jobs after this one:
#         HM_IMAGE        <registry image>@<the index's digest>, which every tag names
#         HM_IMAGE_AMD64  <registry image>@<the linux/amd64 image's digest>
#         HM_IMAGE_ARM64  <registry image>@<the linux/arm64 image's digest>
#         HM_IMAGE_TAGS   the tags written, comma-separated
#       container-image/index.json and manifest-<arch>.json, what was pushed, and
#       container-image/tags.txt, each tag and whether it was written or left, and why.
#
# Only while the project's CI/CD variable HM_PUBLISH_IMAGE is 1, read as the job runs,
# and only at the group's path, registry.gitlab.com/hmailserver/hmailserver
# (hmailserver/docs/ContainerImage.md, "Publishing").
#
# Nothing is pushed until both platforms' images are here, each tarball the one its
# job checked (its SHA256SUMS.txt), for the architecture and the version it must be,
# and each manifest naming the configuration its job checked (image.json). The index
# is assembled in an OCI image layout of the job's own and read back - two entries,
# linux/amd64 and linux/arm64, each the digest of its platform's manifest - before
# anything is sent. Then one copy pushes both manifests by digest and the index under
# :<version>, and the other tags are put on the same index. Every tag written is read
# back from the registry, and must name the index, and the index each platform's
# manifest.
#
# :<major>.<minor> and :latest only move forward. Each is put on the index when it does
# not exist yet, or when the image it names now says (its label
# org.opencontainers.image.version, read from its linux/amd64 image) that it is this
# version - a re-run of this release - or an older one, compared number by number, so
# that 6.3.10 is newer than 6.3.9. It is left where it is when that image is a newer
# version, and when its version cannot be read (no label, or one that is not
# <major>.<minor>.<patch>), which the log says. So a release whose image jobs run after
# a newer release's - started late, or an old pipeline's job run again - still pushes
# :<version>, and never takes :latest or :<major>.<minor> back.
#
# kaniko cannot build two platforms in one run, nor join two images under one index;
# regctl does the joining, as it does the channels' registry mirror. It comes from
# Docker Hub, pinned by digest, and nothing here comes from GitHub.
#
# The registry password is the job's own CI_REGISTRY_PASSWORD, handed to regctl on
# its standard input into a configuration folder of the job's own (HOME), removed at
# the end. It is never on a command line or printed.
set -eu

out=container-image
platforms="amd64 arm64"
pushed=""
work=""
digest_amd64=""
digest_arm64=""

fail() {
  echo "CONTAINER-IMAGE FAILED: $*"
  if [ -n "$pushed" ]; then
    echo "These tags were written, or may have been, before this step failed, and nothing will sign,"
    echo "scan or start what they name; delete any of them the container registry shows"
    echo "(Deploy > Container registry):"
    echo "$pushed" | tr ' ' '\n' | sed '/^$/d; s/^/  /'
  fi
  exit 1
}

# The digest of a platform's image, once it is in the layout.
platform_digest() {
  case "$1" in
    amd64) printf '%s\n' "$digest_amd64" ;;
    arm64) printf '%s\n' "$digest_arm64" ;;
  esac
}

# A version whose tags move: <major>.<minor>.<patch>, each a decimal number without a
# leading zero and of at most nine digits, so that the shell compares it as a number.
plain='(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})'

# version_order A B: "newer", "same" or "older", A against B, each a plain version,
# compared number by number: 6.3.10 is newer than 6.3.9, 7.0.0 than 6.10.10.
# (build/check-release-chain.py runs this function against its cases.)
version_order() {
  a="$1."
  b="$2."
  while [ -n "$a" ] || [ -n "$b" ]; do
    x="${a%%.*}"
    y="${b%%.*}"
    a="${a#*.}"
    b="${b#*.}"
    if [ "${x:-0}" -gt "${y:-0}" ]; then
      echo newer
      return 0
    fi
    if [ "${x:-0}" -lt "${y:-0}" ]; then
      echo older
      return 0
    fi
  done
  echo same
}

# move TAG WHY: TAG put on the index, and said. keep TAG WHY: TAG left where it is, and said.
written=""
kept=""
move() {
  echo "moving $repository:$1 to the index: $2"
  printf '%s  written: %s\n' "$1" "$2" >> "$out/tags.txt"
  pushed="$pushed $repository:$1"
  regctl image copy "$repository@$index" "$repository:$1" || fail "$repository:$1 could not be put on $index"
  written="$written $1"
}
keep() {
  echo "NOT MOVED: $repository:$1 stays where it is: $2"
  printf '%s  left: %s\n' "$1" "$2" >> "$out/tags.txt"
  kept="$kept $1"
}

# The switch, read as the job runs: the jobs exist in every v* tag's pipeline, and
# nothing is pushed until the project's variable says so.
if [ "${HM_PUBLISH_IMAGE:-}" != 1 ]; then
  echo "REFUSED: the project's CI/CD variable HM_PUBLISH_IMAGE is not 1, so no image is pushed."
  echo "It is set by the maintainers for the image at registry.gitlab.com/hmailserver/hmailserver"
  echo "(hmailserver/docs/ContainerImage.md, 'Publishing'); then run the image jobs again."
  exit 3
fi

tag="${CI_COMMIT_TAG:-}"
case "$tag" in
  v[0-9]*) ;;
  *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
esac
version="${tag#v}"
printf '%s\n' "$version" | grep -Eqx '[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}' || fail "$version is not a container image tag"

# Where the image goes: the project's own repository at the group's path, which the
# Compose file and the chart name, and never anywhere else (build/ci/linux-image.sh says why).
repository="${CI_REGISTRY_IMAGE:-}"
case "$repository" in
  registry.gitlab.com/hmailserver/hmailserver) ;;
  *) fail "the project's registry image is '$repository', not registry.gitlab.com/hmailserver/hmailserver, which the Compose file and the chart name" ;;
esac
# The tags that move with a release without a suffix, each only forward (move, keep).
moving=""
if printf '%s\n' "$version" | grep -Eqx "$plain"; then
  moving="${version%.*} latest"
fi
[ -n "${CI_REGISTRY:-}" ] && [ -n "${CI_REGISTRY_USER:-}" ] && [ -n "${CI_REGISTRY_PASSWORD:-}" ] \
  || fail "the job has no CI_REGISTRY, CI_REGISTRY_USER or CI_REGISTRY_PASSWORD: the project's container registry is off"
command -v regctl > /dev/null 2>&1 || fail "there is no regctl in this image: the job runs in regclient/regctl's -alpine image"
regctl version 2>&1 | grep -E '^VCS(Tag|Commit):' || true

rm -rf "$out"
mkdir -p "$out"
# regctl keeps its logins in $HOME/.regctl and reads Docker's configuration there too:
# a home of the job's own, and no REGCTL_CONFIG naming another file.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
HOME="$work/home"
mkdir -p "$HOME"
export HOME
unset REGCTL_CONFIG DOCKER_CONFIG 2> /dev/null || true
layout="ocidir://$work/layout"

# Each platform's image, from its job's tarball, into the layout.
for arch in $platforms; do
  dir="container-image-$arch"
  for file in hmailserver-image.tar image.json SHA256SUMS.txt; do
    [ -f "$dir/$file" ] || fail "no $dir/$file: container-image-$arch's artifact did not arrive"
  done
  ( cd "$dir" && sha256sum -c SHA256SUMS.txt ) > "$out/sums-$arch.txt" 2>&1 \
    || { cat "$out/sums-$arch.txt"; fail "$dir's tarball or image.json is not what container-image-$arch checked"; }
  grep -q "\"architecture\":\"$arch\"" "$dir/image.json" || fail "$dir's image is not for architecture $arch"
  grep -q '"os":"linux"' "$dir/image.json" || fail "$dir's image is not for os linux"
  grep -q "\"org.opencontainers.image.version\":\"$version\"" "$dir/image.json" || fail "$dir's image is not version $version"
  regctl image import "$layout:$arch" "$dir/hmailserver-image.tar" || fail "regctl could not read $dir/hmailserver-image.tar"
  regctl manifest get "$layout:$arch" --format raw-body > "$out/manifest-$arch.json" || fail "the linux/$arch manifest could not be read back"
  config="sha256:$(sha256sum "$dir/image.json" | cut -d' ' -f1)"
  grep -q "\"config\":{[^}]*\"digest\":\"$config\"" "$out/manifest-$arch.json" \
    || fail "the linux/$arch manifest does not name the configuration container-image-$arch checked ($config)"
  digest=$(regctl image digest "$layout:$arch") || fail "the linux/$arch image has no digest"
  printf '%s\n' "$digest" | grep -Eqx 'sha256:[0-9a-f]{64}' || fail "the linux/$arch image's digest is '$digest'"
  case "$arch" in
    amd64) digest_amd64="$digest" ;;
    arm64) digest_arm64="$digest" ;;
  esac
  echo "linux/$arch: $digest (configuration $config)"
done

# The index: a Docker manifest list, like the two manifests it names, each entry with
# the platform regctl reads from that image's configuration.
regctl index create "$layout:$version" --media-type application/vnd.docker.distribution.manifest.list.v2+json \
  --ref "$layout:amd64" --ref "$layout:arm64" || fail "regctl could not make the index"
regctl manifest get "$layout:$version" --format raw-body > "$out/index.json" || fail "the index could not be read back"
index=$(regctl image digest "$layout:$version") || fail "the index has no digest"
printf '%s\n' "$index" | grep -Eqx 'sha256:[0-9a-f]{64}' || fail "the index's digest is '$index'"
[ "$(grep -o '"digest":"sha256:[0-9a-f]*"' "$out/index.json" | wc -l)" -eq 2 ] \
  || fail "the index names $(grep -o '"digest":"sha256:[0-9a-f]*"' "$out/index.json" | wc -l) manifests, not the two platforms'"
for arch in $platforms; do
  want=$(platform_digest "$arch")
  got=$(regctl image digest --platform "linux/$arch" "$layout:$version") || fail "the index has no linux/$arch entry"
  [ "$got" = "$want" ] || fail "the index's linux/$arch entry is $got, not $want"
done
echo "index: $index, linux/amd64 and linux/arm64"

# The push, now that everything is checked.
printf '%s' "$CI_REGISTRY_PASSWORD" | regctl registry login "$CI_REGISTRY" --user "$CI_REGISTRY_USER" --pass-stdin > /dev/null \
  || fail "regctl could not log in to $CI_REGISTRY with the job's credentials"
echo "pushing to: $repository:$version; moved forward only:$(for t in $moving; do printf ' %s:%s' "$repository" "$t"; done)"
# Both platforms' manifests, by digest, and the index under the version's tag.
pushed="$repository:$version"
regctl image copy "$layout:$version" "$repository:$version" || fail "the index and its two images could not be pushed as $repository:$version"
written="$version"
printf '%s  written: this release\n' "$version" > "$out/tags.txt"

# :<major>.<minor> and :latest, each only forward: made when it does not exist, moved
# when the image it names is this version or an older one, and otherwise left, as the
# header says. What a tag names now is read from the registry: its list of tags, and
# the version label of the linux/amd64 image of what the tag names.
if [ -n "$moving" ]; then
  listed=0
  if regctl tag ls "$repository" > "$work/tags" 2> "$work/tags.log"; then
    listed=1
  else
    cat "$work/tags.log"
  fi
  for t in $moving; do
    if [ "$listed" != 1 ]; then
      keep "$t" "the repository's tags could not be listed, so whether it names a newer version is not known"
      continue
    fi
    if ! grep -qxF "$t" "$work/tags"; then
      move "$t" "it did not exist yet"
      continue
    fi
    current=$(regctl image config "$repository:$t" --platform linux/amd64 \
      --format '{{ index .Config.Labels "org.opencontainers.image.version" }}' 2> "$work/config.log") || current=""
    case "$current" in
      *"
"*) current="" ;;
    esac
    if ! printf '%s\n' "$current" | grep -Eqx "$plain"; then
      cat "$work/config.log"
      keep "$t" "the image it names has no version this job can read (org.opencontainers.image.version '$current'), so whether it is newer is not known; put it on $repository@$index by hand if it should name $version"
      continue
    fi
    case "$(version_order "$version" "$current")" in
      newer) move "$t" "it named $current, older than $version" ;;
      same) move "$t" "it named $current, this version: a re-run of this release" ;;
      older) keep "$t" "it names $current, newer than $version" ;;
      *) keep "$t" "$version and $current could not be compared" ;;
    esac
  done
fi

# Read back from the registry: every tag written names the index, and the index each
# platform's manifest.
for t in $written; do
  got=$(regctl image digest "$repository:$t") || fail "$repository:$t could not be read back"
  [ "$got" = "$index" ] || fail "$repository:$t names $got, not the index pushed, $index"
done
for arch in $platforms; do
  want=$(platform_digest "$arch")
  got=$(regctl image digest --platform "linux/$arch" "$repository@$index") || fail "the pushed index has no linux/$arch entry"
  [ "$got" = "$want" ] || fail "the pushed index's linux/$arch entry is $got, not $want"
  regctl manifest head "$repository@$want" > /dev/null || fail "$repository@$want, the linux/$arch image, is not in the registry"
done
regctl registry logout "$CI_REGISTRY" > /dev/null 2>&1 || true

# What the jobs after this one read (artifacts:reports:dotenv), one KEY=value a line:
# container_scanning and container_scanning_arm64 each take their platform's image.
{
  echo "HM_IMAGE=$repository@$index"
  echo "HM_IMAGE_AMD64=$repository@$digest_amd64"
  echo "HM_IMAGE_ARM64=$repository@$digest_arm64"
  echo "HM_IMAGE_TAGS=$(printf '%s' "$written" | tr ' ' ',')"
} > "$out/image.env"
cat "$out/image.env"
cat "$out/tags.txt"
echo "IMAGE PUSHED: $repository@$index (linux/amd64 $digest_amd64, linux/arm64 $digest_arm64), as$(for t in $written; do printf ' %s:%s' "$repository" "$t"; done)."
if [ -n "$kept" ]; then
  echo "NOT MOVED:$(for t in $kept; do printf ' %s:%s' "$repository" "$t"; done) (container-image/tags.txt says why)."
fi
