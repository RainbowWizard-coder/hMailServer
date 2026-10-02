#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The container-image-artifacthub job of .gitlab/ci/container-image.yml: Artifact Hub's
# metadata for the image's repository - the tree's
# hmailserver/source/Server/platform/packaging/docker/artifacthub-repo.yml, as it is -
# pushed to the project's container registry under the tag artifacthub.io, where
# Artifact Hub looks for it (its documentation, "Container images repositories"). The
# repository ID it carries gives the image's listing its Verified publisher label.
# Run in regctl's own image (docker.io's regclient/regctl, the -alpine variant
# container-image runs in), from the root of the checkout:
#
#   sh build/ci/container-image-artifacthub.sh
#
# In a protected v* tag's pipeline it runs after container-image has pushed the
# release's image; in the default branch's, a maintainer starts it by hand, to push the
# file once the ID is committed without waiting for the next release.
#
# Out:  container-image-artifacthub/result.txt        what was done, or why nothing was
#       container-image-artifacthub/artifacthub-repo.yml  the file pushed, as read back
#       container-image-artifacthub/manifest.json     its manifest, as read back
#
# What is pushed is an OCI artifact of the shape Artifact Hub documents for
# oras push: an image manifest whose configuration is empty, of the media type
# application/vnd.cncf.artifacthub.config.v1+yaml, and whose one layer is the file,
# of the media type application/vnd.cncf.artifacthub.repository-metadata.layer.v1.yaml,
# titled artifacthub-repo.yml. Artifact Hub reads that layer. regctl pushes it (artifact
# put), as Artifact Hub's documentation allows; ORAS would have to come from GitHub.
#
# Nothing is pushed while the file holds no repository ID - Artifact Hub has not given
# one until the repository is added there, and a file without it tells Artifact Hub
# nothing - nor when the file holds anything but comments, blank lines and one line
# "repositoryID: <a UUID>". Only while the project's CI/CD variable HM_PUBLISH_IMAGE is
# 1, read as the job runs, and only at the group's path,
# registry.gitlab.com/hmailserver/hmailserver, as container-image-push.sh. No other tag
# is written. The tag is read back: one manifest of that shape, whose layer is the
# file byte for byte.
#
# The registry password is the job's own CI_REGISTRY_PASSWORD, handed to regctl on its
# standard input into a configuration folder of the job's own (HOME), removed at the
# end. It is never on a command line or printed.
set -eu

out=container-image-artifacthub
metadata=hmailserver/source/Server/platform/packaging/docker/artifacthub-repo.yml
metadata_tag=artifacthub.io
config_type=application/vnd.cncf.artifacthub.config.v1+yaml
layer_type=application/vnd.cncf.artifacthub.repository-metadata.layer.v1.yaml
uuid='[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}'
pushed=""

fail() {
  echo "CONTAINER-IMAGE-ARTIFACTHUB FAILED: $*"
  if [ -n "$pushed" ]; then
    echo "$pushed was written, or may have been, before this step failed: what it names may not be the"
    echo "tree's file. Run the job again, or delete the tag (Deploy > Container registry)."
  fi
  exit 1
}

# The switch, read as the job runs, as the other image jobs read it.
if [ "${HM_PUBLISH_IMAGE:-}" != 1 ]; then
  echo "REFUSED: the project's CI/CD variable HM_PUBLISH_IMAGE is not 1, so nothing is pushed."
  echo "It is set by the maintainers for the image at registry.gitlab.com/hmailserver/hmailserver"
  echo "(hmailserver/docs/ContainerImage.md, 'Publishing'); then run this job again."
  exit 3
fi

# Which pipeline: a protected v* tag's, or the protected default branch's.
[ "${CI_COMMIT_REF_PROTECTED:-}" = true ] || fail "this pipeline's ref is not protected"
tag="${CI_COMMIT_TAG:-}"
branch="${CI_COMMIT_BRANCH:-}"
if [ -n "$tag" ]; then
  case "$tag" in
    v[0-9]*) where="the tag $tag" ;;
    *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
  esac
elif [ -n "$branch" ] && [ "$branch" = "${CI_DEFAULT_BRANCH:-}" ]; then
  where="the default branch $branch"
else
  fail "this is neither a v* tag's pipeline nor the default branch's (CI_COMMIT_BRANCH is '$branch', CI_DEFAULT_BRANCH '${CI_DEFAULT_BRANCH:-}')"
fi

# Where: the project's own repository at the group's path, which Artifact Hub is given,
# and never anywhere else (build/ci/container-image-push.sh).
repository="${CI_REGISTRY_IMAGE:-}"
case "$repository" in
  registry.gitlab.com/hmailserver/hmailserver) ;;
  *) fail "the project's registry image is '$repository', not registry.gitlab.com/hmailserver/hmailserver, the repository Artifact Hub lists" ;;
esac

rm -rf "$out"
mkdir -p "$out"

# The file: only comments, blank lines and at most one repositoryID line, in LF.
[ -f "$metadata" ] || fail "there is no $metadata"
[ "$(wc -c < "$metadata")" -le 4096 ] || fail "$metadata is larger than 4 KiB"
! grep -q "$(printf '\r')" "$metadata" || fail "$metadata has a carriage return: it is pushed byte for byte, with LF line endings"
unknown=$(grep -Ev "^[[:space:]]*\$|^#|^repositoryID: $uuid\$" "$metadata" || true)
[ -z "$unknown" ] || fail "$metadata has a line that is not a comment, a blank line or 'repositoryID: <a UUID, lower case>': $(printf '%s' "$unknown" | head -n 1)"
ids=$(grep -Ec "^repositoryID: " "$metadata" || true)
[ "$ids" -le 1 ] || fail "$metadata names $ids repository IDs, not one"
if [ "$ids" = 0 ]; then
  {
    echo "Nothing pushed for $where: $metadata holds no repository ID yet."
    echo "It is written there once the repository oci://$repository has been added to the organization"
    echo "hmailserver on Artifact Hub (hmailserver/docs/ContainerImage.md, 'Artifact Hub'), and pushed by this"
    echo "job in the default branch's pipeline, by hand, and in every release's."
  } | tee "$out/result.txt"
  exit 0
fi
id=$(sed -n 's/^repositoryID: //p' "$metadata")
echo "repository ID: $id, from $metadata, for $where"

[ -n "${CI_REGISTRY:-}" ] && [ -n "${CI_REGISTRY_USER:-}" ] && [ -n "${CI_REGISTRY_PASSWORD:-}" ] \
  || fail "the job has no CI_REGISTRY, CI_REGISTRY_USER or CI_REGISTRY_PASSWORD: the project's container registry is off"
command -v regctl > /dev/null 2>&1 || fail "there is no regctl in this image: the job runs in regclient/regctl's -alpine image"
regctl version 2>&1 | grep -E '^VCS(Tag|Commit):' || true

# regctl keeps its logins in $HOME/.regctl and reads Docker's configuration there too:
# a home of the job's own, and no REGCTL_CONFIG naming another file.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
HOME="$work/home"
mkdir -p "$HOME"
export HOME
unset REGCTL_CONFIG DOCKER_CONFIG 2> /dev/null || true
# The file under the name Artifact Hub's documentation pushes it with, which the layer's
# title annotation carries.
mkdir -p "$work/file"
cp "$metadata" "$work/file/artifacthub-repo.yml"
size=$(wc -c < "$metadata" | tr -d ' ')
digest="sha256:$(sha256sum "$metadata" | cut -d' ' -f1)"
: > "$work/empty"

printf '%s' "$CI_REGISTRY_PASSWORD" | regctl registry login "$CI_REGISTRY" --user "$CI_REGISTRY_USER" --pass-stdin > /dev/null \
  || fail "regctl could not log in to $CI_REGISTRY with the job's credentials"
echo "pushing $metadata ($size bytes, $digest) to $repository:$metadata_tag"
pushed="$repository:$metadata_tag"
( cd "$work/file" && regctl artifact put --config-type "$config_type" --config-file "$work/empty" \
    --file-media-type "$layer_type" --file artifacthub-repo.yml --file-title \
    "$repository:$metadata_tag" ) || fail "regctl could not push $repository:$metadata_tag"

# Read back: one image manifest, the empty configuration of Artifact Hub's type, and one
# layer of its type that is the file.
regctl manifest get "$repository:$metadata_tag" --format raw-body > "$out/manifest.json" \
  || fail "$repository:$metadata_tag could not be read back"
tr -d ' \n\r\t' < "$out/manifest.json" > "$work/manifest"
grep -q '"mediaType":"application/vnd.oci.image.manifest.v1+json"' "$work/manifest" \
  || fail "$repository:$metadata_tag is not an OCI image manifest: $(cat "$work/manifest")"
grep -q "\"config\":{\"mediaType\":\"$config_type\",\"digest\":\"sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"size\":0}" "$work/manifest" \
  || fail "$repository:$metadata_tag's configuration is not the empty one of $config_type: $(cat "$work/manifest")"
[ "$(grep -o '"mediaType":"[^"]*"' "$work/manifest" | grep -c "\"$layer_type\"")" = 1 ] \
  || fail "$repository:$metadata_tag has not exactly one layer of $layer_type: $(cat "$work/manifest")"
grep -q "\"layers\":\[{\"mediaType\":\"$layer_type\",\"digest\":\"$digest\",\"size\":$size,\"annotations\":{\"org.opencontainers.image.title\":\"artifacthub-repo.yml\"}}\]" "$work/manifest" \
  || fail "$repository:$metadata_tag's one layer is not $metadata ($digest, $size bytes, titled artifacthub-repo.yml): $(cat "$work/manifest")"
regctl artifact get "$repository:$metadata_tag" --file-media-type "$layer_type" > "$out/artifacthub-repo.yml" \
  || fail "the layer of $repository:$metadata_tag could not be read back"
cmp -s "$out/artifacthub-repo.yml" "$metadata" || fail "the layer of $repository:$metadata_tag is not $metadata byte for byte"
manifest_digest=$(regctl image digest "$repository:$metadata_tag") || fail "$repository:$metadata_tag has no digest"
regctl registry logout "$CI_REGISTRY" > /dev/null 2>&1 || true

{
  echo "Pushed for $where: $repository:$metadata_tag = $repository@$manifest_digest"
  echo "  configuration: $config_type, empty"
  echo "  layer: $layer_type, $digest, $size bytes, artifacthub-repo.yml"
  echo "  repositoryID: $id"
} | tee "$out/result.txt"
echo "ARTIFACT HUB METADATA PUSHED: $repository:$metadata_tag, repository ID $id."
