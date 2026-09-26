#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The container-image-sbom job of .gitlab/ci/container-image.yml: the software bill
# of materials of the release's container image, which container-image pushed, for
# container-image-sign to attest to the image with cosign (OpenSSF Baseline
# OSPS-QA-02.02: every compiled release asset is delivered with an SBOM; for the
# image, the SBOM travels with it in the registry as a signed attestation).
#
#   bash build/ci/container-image-sbom.sh
#
# Run in the image of build/ci/linux.Dockerfile (syft, python3), from the root of the
# checkout of the tag.
#
# In:   HM_IMAGE, from container-image's dotenv report: <registry image>@sha256:<digest>.
#       release-signed/, release-sign's artifact: the .deb the image is built from and
#       that .deb's own SBOM (build/asset-sbom.py), both as the release carries them.
# Out:  container-image-sbom/hmailserver-image.cdx.json   the image's SBOM
#       container-image-sbom/image.syft.cdx.json          what Syft found in the image
#
# Syft reads the image from the registry by its digest, with the job's own registry
# credentials (CI_REGISTRY_USER and CI_REGISTRY_PASSWORD, which the job token has for
# this project alone), handed to it in the environment and never printed. It lists
# every distribution package in the image, the hmailserver .deb among them;
# build/asset-sbom.py adds what the .deb's own SBOM names and Syft cannot see inside
# the binary (Boost, linked in, and QuickJS, compiled in), and names the image by its
# digest as the document's subject.
set -euo pipefail

out=container-image-sbom
signed="${HM_IMAGE_DEB_DIR:-release-signed}"

fail() {
  echo "CONTAINER-IMAGE-SBOM FAILED: $*"
  exit 1
}

tag="${CI_COMMIT_TAG:-}"
case "$tag" in
  v[0-9]*) ;;
  *) fail "this is not a v* tag's pipeline (CI_COMMIT_TAG is '$tag')" ;;
esac
version="${tag#v}"
image="${HM_IMAGE:-}"
registry_image="${CI_REGISTRY_IMAGE:-}"
[ -n "$registry_image" ] || fail "the job has no CI_REGISTRY_IMAGE: the project's container registry is off"
printf '%s\n' "$image" | grep -Eqx "${registry_image}@sha256:[0-9a-f]{64}" \
  || fail "HM_IMAGE is '$image', not $registry_image by digest: container-image's dotenv report did not arrive"
[ -n "${CI_REGISTRY:-}" ] && [ -n "${CI_REGISTRY_USER:-}" ] && [ -n "${CI_REGISTRY_PASSWORD:-}" ] \
  || fail "the job has no CI_REGISTRY, CI_REGISTRY_USER or CI_REGISTRY_PASSWORD"

deb="$signed/hmailserver_${version}_amd64.deb"
[ -f "$deb" ] || fail "no $deb: release-sign's artifact did not arrive"
[ -f "$deb.cdx.json" ] || fail "no $deb.cdx.json: the release carries no SBOM of the .deb the image is built from"
python3 build/asset-sbom.py verify "$deb.cdx.json" "$deb" > /dev/null \
  || fail "$deb.cdx.json does not describe $deb"

rm -rf "$out"
mkdir -p "$out"
syft version | head -n 2

# No update check, nothing sent anywhere but the registry read.
export SYFT_CHECK_FOR_APP_UPDATE=false
SYFT_REGISTRY_AUTH_AUTHORITY="$CI_REGISTRY" \
SYFT_REGISTRY_AUTH_USERNAME="$CI_REGISTRY_USER" \
SYFT_REGISTRY_AUTH_PASSWORD="$CI_REGISTRY_PASSWORD" \
  syft scan "registry:$image" --quiet \
    --source-name "$registry_image" --source-version "$version" \
    -o "cyclonedx-json=$out/image.syft.cdx.json" \
  || fail "syft could not read $image from the registry"

python3 build/asset-sbom.py image --syft "$out/image.syft.cdx.json" --deb-sbom "$deb.cdx.json" \
  --image "$image" --version "$version" --out "$out/hmailserver-image.cdx.json" \
  || fail "the image's SBOM could not be made"

# Syft must have found the server's own package in the image, or it did not read it.
python3 -c 'import json, sys; sys.exit(0 if any(c.get("name") == "hmailserver" for c in json.load(open(sys.argv[1])).get("components") or []) else 1)' \
    "$out/image.syft.cdx.json" \
  || fail "Syft found no hmailserver package in $image: it did not read the image the .deb went into"
ls -la "$out"
echo "SBOM MADE: $out/hmailserver-image.cdx.json describes $image."
